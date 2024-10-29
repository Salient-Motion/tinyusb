/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && defined(TUP_USBIP_DWC2)

// Debug level for DWC2
#define DWC2_DEBUG    2

#include "host/hcd.h"
#include "dwc2_common.h"

// Max number of endpoints application can open, can be larger than DWC2_CHANNEL_COUNT_MAX
#ifndef CFG_TUH_DWC2_ENDPOINT_MAX
#define CFG_TUH_DWC2_ENDPOINT_MAX 16
#endif

#define DWC2_CHANNEL_COUNT_MAX    16 // absolute max channel count
#define DWC2_CHANNEL_COUNT(_dwc2) tu_min8((_dwc2)->ghwcfg2_bm.num_host_ch + 1, DWC2_CHANNEL_COUNT_MAX)

TU_VERIFY_STATIC(CFG_TUH_DWC2_ENDPOINT_MAX <= 255, "currently only use 8-bit for index");

enum {
  HPRT_W1C_MASK = HPRT_CONN_DETECT | HPRT_ENABLE | HPRT_ENABLE_CHANGE | HPRT_OVER_CURRENT_CHANGE
};

enum {
  HCD_XFER_ERROR_MAX = 3
};

enum {
  HCD_XFER_STATE_UNALLOCATED = 0,
  HCD_XFER_STATE_ACTIVE = 1,
  HCD_XFER_STATE_DISABLING = 2,
};

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------

// Host driver struct for each opened endpoint
typedef struct {
  union {
    uint32_t hcchar;
    dwc2_channel_char_t hcchar_bm;
  };
  union {
    uint32_t hcsplt;
    dwc2_channel_split_t hcsplt_bm;
  };

  uint8_t next_data_toggle;
  // uint8_t resv[3];
} hcd_endpoint_t;

// Additional info for each channel when it is active
typedef struct {
  volatile uint8_t state;
  uint8_t err_count;
  uint8_t* buffer;
  uint16_t total_bytes;
} hcd_xfer_t;

typedef struct {
  hcd_xfer_t xfer[DWC2_CHANNEL_COUNT_MAX];
  hcd_endpoint_t edpt[CFG_TUH_DWC2_ENDPOINT_MAX];
} hcd_data_t;

hcd_data_t _hcd_data;

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------
TU_ATTR_ALWAYS_INLINE static inline tusb_speed_t convert_hprt_speed(uint32_t hprt_speed) {
  tusb_speed_t speed;
  switch(hprt_speed) {
    case HPRT_SPEED_HIGH: speed = TUSB_SPEED_HIGH; break;
    case HPRT_SPEED_FULL: speed = TUSB_SPEED_FULL; break;
    case HPRT_SPEED_LOW : speed = TUSB_SPEED_LOW ; break;
    default:
      speed = TUSB_SPEED_INVALID;
      TU_BREAKPOINT();
    break;
  }
  return speed;
}

TU_ATTR_ALWAYS_INLINE static inline bool dma_host_enabled(const dwc2_regs_t* dwc2) {
  (void) dwc2;
  // Internal DMA only
  return CFG_TUH_DWC2_DMA && dwc2->ghwcfg2_bm.arch == GHWCFG2_ARCH_INTERNAL_DMA;
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t request_queue_avail(const dwc2_regs_t* dwc2, bool is_period) {
  if (is_period) {
    return dwc2->hptxsts_bm.req_queue_available;
  } else {
    return dwc2->hnptxsts_bm.req_queue_available;
  }
}

// Check if channel is periodic (interrupt/isochronous)
TU_ATTR_ALWAYS_INLINE static inline bool channel_is_periodic(dwc2_channel_char_t hcchar_bm) {
  return hcchar_bm.ep_type == HCCHAR_EPTYPE_INTERRUPT || hcchar_bm.ep_type == HCCHAR_EPTYPE_ISOCHRONOUS;
}

// Find a free channel for new transfer
TU_ATTR_ALWAYS_INLINE static inline uint8_t channel_alloc(dwc2_regs_t* dwc2) {
  const uint8_t max_channel = DWC2_CHANNEL_COUNT(dwc2);
  for (uint8_t ch_id = 0; ch_id < max_channel; ch_id++) {
    hcd_xfer_t* xfer = &_hcd_data.xfer[ch_id];
    if (HCD_XFER_STATE_UNALLOCATED == xfer->state) {
      tu_memclr(xfer, sizeof(hcd_xfer_t));
      xfer->state = HCD_XFER_STATE_ACTIVE;
      return ch_id;
    }
  }
  return TUSB_INDEX_INVALID_8;
}

TU_ATTR_ALWAYS_INLINE static inline void channel_dealloc(dwc2_regs_t* dwc2, uint8_t ch_id) {
  (void) dwc2;
  _hcd_data.xfer[ch_id].state = HCD_XFER_STATE_UNALLOCATED;
}

// Find currently enabled channel. Note: EP0 is bidirectional
TU_ATTR_ALWAYS_INLINE static inline uint8_t channel_find_enabled(dwc2_regs_t* dwc2, uint8_t dev_addr, uint8_t ep_num, uint8_t ep_dir) {
  const uint8_t max_channel = DWC2_CHANNEL_COUNT(dwc2);
  for (uint8_t ch_id = 0; ch_id < max_channel; ch_id++) {
    if (_hcd_data.xfer[ch_id].state != HCD_XFER_STATE_UNALLOCATED) {
      const dwc2_channel_char_t hcchar_bm = dwc2->channel[ch_id].hcchar_bm;
      if (hcchar_bm.dev_addr == dev_addr && hcchar_bm.ep_num == ep_num && (ep_num == 0 || hcchar_bm.ep_dir == ep_dir)) {
        return ch_id;
      }
    }
  }
  return TUSB_INDEX_INVALID_8;
}

// Find a endpoint that is opened previously with hcd_edpt_open()
// Note: EP0 is bidirectional
TU_ATTR_ALWAYS_INLINE static inline uint8_t edpt_find_opened(uint8_t dev_addr, uint8_t ep_num, uint8_t ep_dir) {
  for (uint8_t i = 0; i < (uint8_t)CFG_TUH_DWC2_ENDPOINT_MAX; i++) {
    const dwc2_channel_char_t* hcchar_bm = &_hcd_data.edpt[i].hcchar_bm;
    if (hcchar_bm->enable && hcchar_bm->dev_addr == dev_addr &&
        hcchar_bm->ep_num == ep_num && (ep_num == 0 || hcchar_bm->ep_dir == ep_dir)) {
      return i;
    }
  }
  return TUSB_INDEX_INVALID_8;
}

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------

/* USB Data FIFO Layout

  The FIFO is split up into
  - EPInfo: for storing DMA metadata (check dcd_dwc2.c for more details)
  - 1 RX FIFO: for receiving data
  - 1 TX FIFO for non-periodic (NPTX)
  - 1 TX FIFO for periodic (PTX)

  We allocated TX FIFO from top to bottom (using top pointer), this to allow the RX FIFO to grow dynamically which is
  possible since the free space is located between the RX and TX FIFOs.

   ----------------- ep_fifo_size
  |    HCDMAn    |
  |--------------|-- gdfifocfg.EPINFOBASE (max is ghwcfg3.dfifo_depth)
  | Non-Periodic |
  |   TX FIFO    |
  |--------------|--- GNPTXFSIZ.addr (fixed size)
  |   Periodic   |
  |   TX FIFO    |
  |--------------|--- HPTXFSIZ.addr (expandable downward)
  |    FREE      |
  |              |
  |--------------|-- GRXFSIZ (expandable upward)
  |  RX FIFO     |
  ---------------- 0
*/

/* Programming Guide 2.1.2 FIFO RAM allocation
 * RX
 * - Largest-EPsize/4 + 2 (status info). recommended x2 if high bandwidth or multiple ISO are used.
 * - 2 for transfer complete and channel halted status
 * - 1 for each Control/Bulk out endpoint to Handle NAK/NYET (i.e max is number of host channel)
 *
 * TX non-periodic (NPTX)
 * - At least largest-EPsize/4, recommended x2
 *
 * TX periodic (PTX)
 * - At least largest-EPsize*MulCount/4 (MulCount up to 3 for high-bandwidth ISO/interrupt)
*/
static void dfifo_host_init(uint8_t rhport) {
  const dwc2_controller_t* dwc2_controller = &_dwc2_controller[rhport];
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  // Scatter/Gather DMA mode is not yet supported. Buffer DMA only need 1 words per channel
  const bool is_dma = dma_host_enabled(dwc2);
  uint16_t dfifo_top = dwc2_controller->ep_fifo_size/4;
  if (is_dma) {
    dfifo_top -= dwc2->ghwcfg2_bm.num_host_ch;
  }

  // fixed allocation for now, improve later:
    // - ptx_largest is limited to 256 for FS since most FS core only has 1024 bytes total
  bool is_highspeed = dwc2_core_is_highspeed(dwc2, TUSB_ROLE_HOST);
  uint32_t nptx_largest = is_highspeed ? TUSB_EPSIZE_BULK_HS/4 : TUSB_EPSIZE_BULK_FS/4;
  uint32_t ptx_largest = is_highspeed ? TUSB_EPSIZE_ISO_HS_MAX/4 : 256/4;

  uint16_t nptxfsiz = 2 * nptx_largest;
  uint16_t rxfsiz = 2 * (ptx_largest + 2) + dwc2->ghwcfg2_bm.num_host_ch;
  TU_ASSERT(dfifo_top >= (nptxfsiz + rxfsiz),);
  uint16_t ptxfsiz = dfifo_top - (nptxfsiz + rxfsiz);

  dwc2->gdfifocfg = (dfifo_top << GDFIFOCFG_EPINFOBASE_SHIFT) | dfifo_top;

  dfifo_top -= rxfsiz;
  dwc2->grxfsiz = rxfsiz;

  dfifo_top -= nptxfsiz;
  dwc2->gnptxfsiz = tu_u32_from_u16(nptxfsiz, dfifo_top);

  dfifo_top -= ptxfsiz;
  dwc2->hptxfsiz = tu_u32_from_u16(ptxfsiz, dfifo_top);
}

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

// optional hcd configuration, called by tuh_configure()
bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param) {
  (void) rhport;
  (void) cfg_id;
  (void) cfg_param;

  return true;
}

// Initialize controller to host mode
bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void) rh_init;
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  tu_memclr(&_hcd_data, sizeof(_hcd_data));

  // Core Initialization
  const bool is_highspeed = dwc2_core_is_highspeed(dwc2, TUSB_ROLE_HOST);
  const bool is_dma = dma_host_enabled(dwc2);
  TU_ASSERT(dwc2_core_init(rhport, is_highspeed, is_dma));

  //------------- 3.1 Host Initialization -------------//

  // FS/LS PHY Clock Select
  uint32_t hcfg = dwc2->hcfg;
  if (is_highspeed) {
    hcfg &= ~HCFG_FSLS_ONLY;
  } else {
    hcfg &= ~HCFG_FSLS_ONLY; // since we are using FS PHY
    hcfg &= ~HCFG_FSLS_PHYCLK_SEL;

    if (dwc2->ghwcfg2_bm.hs_phy_type == GHWCFG2_HSPHY_ULPI &&
        dwc2->ghwcfg2_bm.fs_phy_type == GHWCFG2_FSPHY_DEDICATED) {
      // dedicated FS PHY with 48 mhz
      hcfg |= HCFG_FSLS_PHYCLK_SEL_48MHZ;
    } else {
      // shared HS PHY running at full speed
      hcfg |= HCFG_FSLS_PHYCLK_SEL_30_60MHZ;
    }
  }
  dwc2->hcfg = hcfg;

  // Enable HFIR reload

  // force host mode and wait for mode switch
  dwc2->gusbcfg = (dwc2->gusbcfg & ~GUSBCFG_FDMOD) | GUSBCFG_FHMOD;
  while( (dwc2->gintsts & GINTSTS_CMOD) != GINTSTS_CMODE_HOST) {}

  // configure fixed-allocated fifo scheme
  dfifo_host_init(rhport);

  dwc2->hprt = HPRT_W1C_MASK; // clear all write-1-clear bits
  dwc2->hprt = HPRT_POWER; // turn on VBUS

  // Enable required interrupts
  dwc2->gintmsk |= GINTSTS_OTGINT | GINTSTS_CONIDSTSCHNG | GINTSTS_HPRTINT | GINTSTS_HCINT;

  // NPTX can hold at least 2 packet, change interrupt level to half-empty
  uint32_t gahbcfg = dwc2->gahbcfg & ~GAHBCFG_TX_FIFO_EPMTY_LVL;
  gahbcfg |= GAHBCFG_GINT;   // Enable global interrupt
  dwc2->gahbcfg = gahbcfg;

  return true;
}

// Enable USB interrupt
void hcd_int_enable (uint8_t rhport) {
  dwc2_int_set(rhport, TUSB_ROLE_HOST, true);
}

// Disable USB interrupt
void hcd_int_disable(uint8_t rhport) {
  dwc2_int_set(rhport, TUSB_ROLE_HOST, false);
}

// Get frame number (1ms)
uint32_t hcd_frame_number(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  return dwc2->hfnum & HFNUM_FRNUM_Msk;
}

//--------------------------------------------------------------------+
// Port API
//--------------------------------------------------------------------+

// Get the current connect status of roothub port
bool hcd_port_connect_status(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  return dwc2->hprt & HPRT_CONN_STATUS;
}

// Reset USB bus on the port. Return immediately, bus reset sequence may not be complete.
// Some port would require hcd_port_reset_end() to be invoked after 10ms to complete the reset sequence.
void hcd_port_reset(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK;
  hprt |= HPRT_RESET;
  dwc2->hprt = hprt;
}

// Complete bus reset sequence, may be required by some controllers
void hcd_port_reset_end(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK; // skip w1c bits
  hprt &= ~HPRT_RESET;
  dwc2->hprt = hprt;
}

// Get port link speed
tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const tusb_speed_t speed = convert_hprt_speed(dwc2->hprt_bm.speed);
  return speed;
}

// HCD closes all opened endpoints belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  (void) rhport;
  for (uint8_t i = 0; i < (uint8_t) CFG_TUH_DWC2_ENDPOINT_MAX; i++) {
    hcd_endpoint_t* edpt = &_hcd_data.edpt[i];
    if (edpt->hcchar_bm.enable && edpt->hcchar_bm.dev_addr == dev_addr) {
      tu_memclr(edpt, sizeof(hcd_endpoint_t));
    }
  }
}

//--------------------------------------------------------------------+
// Endpoints API
//--------------------------------------------------------------------+

// Open an endpoint
bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_endpoint_t* desc_ep) {
  (void) rhport;
  //dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  hcd_devtree_info_t devtree_info;
  hcd_devtree_get_info(dev_addr, &devtree_info);

  // find a free endpoint
  for (uint32_t i = 0; i < CFG_TUH_DWC2_ENDPOINT_MAX; i++) {
    hcd_endpoint_t* edpt = &_hcd_data.edpt[i];
    dwc2_channel_char_t* hcchar_bm = &edpt->hcchar_bm;
    dwc2_channel_split_t* hcsplt_bm = &edpt->hcsplt_bm;

    if (hcchar_bm->enable == 0) {
      hcchar_bm->ep_size = tu_edpt_packet_size(desc_ep);
      hcchar_bm->ep_num = tu_edpt_number(desc_ep->bEndpointAddress);
      hcchar_bm->ep_dir = tu_edpt_dir(desc_ep->bEndpointAddress);
      hcchar_bm->low_speed_dev = (devtree_info.speed == TUSB_SPEED_LOW) ? 1 : 0;
      hcchar_bm->ep_type = desc_ep->bmAttributes.xfer; // ep_type matches TUSB_XFER_*
      hcchar_bm->err_multi_count = 0;
      hcchar_bm->dev_addr = dev_addr;
      hcchar_bm->odd_frame = 0;
      hcchar_bm->disable = 0;
      hcchar_bm->enable = 1;

      hcsplt_bm->hub_port = devtree_info.hub_port;
      hcsplt_bm->hub_addr = devtree_info.hub_addr;
      // TODO not support split transaction yet
      hcsplt_bm->xact_pos = 0;
      hcsplt_bm->split_compl = 0;
      hcsplt_bm->split_en = 0;

      edpt->next_data_toggle = HCTSIZ_PID_DATA0;

      return true;
    }
  }

  return false;
}

// Submit a transfer, when complete hcd_event_xfer_complete() must be invoked
bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t * buffer, uint16_t buflen) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const uint8_t ep_num = tu_edpt_number(ep_addr);
  const uint8_t ep_dir = tu_edpt_dir(ep_addr);
  uint8_t ep_id = edpt_find_opened(dev_addr, ep_num, ep_dir);
  TU_ASSERT(ep_id < CFG_TUH_DWC2_ENDPOINT_MAX);
  hcd_endpoint_t* edpt = &_hcd_data.edpt[ep_id];
  dwc2_channel_char_t* hcchar_bm = &edpt->hcchar_bm;

  uint8_t ch_id = channel_alloc(dwc2);
  TU_ASSERT(ch_id < 16); // all channel are in used
  hcd_xfer_t* xfer = &_hcd_data.xfer[ch_id];
  dwc2_channel_t* channel = &dwc2->channel[ch_id];

  uint32_t hcintmsk = HCINT_NAK | HCINT_XACT_ERR | HCINT_STALL | HCINT_XFER_COMPLETE | HCINT_DATATOGGLE_ERR;
  if (dma_host_enabled(dwc2)) {
    TU_ASSERT(false); // not yet supported
  } else {
    if (hcchar_bm->ep_dir == TUSB_DIR_OUT) {
      hcintmsk |= HCINT_NYET;
    } else {
      hcintmsk |= HCINT_BABBLE_ERR | HCINT_DATATOGGLE_ERR;
    }
  }

  uint16_t packet_count = tu_div_ceil(buflen, hcchar_bm->ep_size);
  if (packet_count == 0) {
    packet_count = 1; // zero length packet still count as 1
  }
  channel->hctsiz = (edpt->next_data_toggle << HCTSIZ_PID_Pos) | (packet_count << HCTSIZ_PKTCNT_Pos) | buflen;

  // Control transfer always start with DATA1 for data and status stage. May has issue with ZLP
  if (edpt->next_data_toggle == HCTSIZ_PID_DATA0 || ep_num == 0) {
    edpt->next_data_toggle = HCTSIZ_PID_DATA1;
  } else {
    edpt->next_data_toggle = HCTSIZ_PID_DATA0;
  }

  // TODO support split transaction
  channel->hcsplt = edpt->hcsplt;

  hcchar_bm->odd_frame = 1 - (dwc2->hfnum & 1); // transfer on next frame
  hcchar_bm->ep_dir = ep_dir;                   // control endpoint can switch direction
  channel->hcchar = (edpt->hcchar & ~HCCHAR_CHENA);    // restore hcchar but don't enable yet

  xfer->buffer = buffer;
  xfer->total_bytes = buflen;

  if (dma_host_enabled(dwc2)) {
    channel->hcdma = (uint32_t) buffer;
  } else {
    bool const is_period = channel_is_periodic(*hcchar_bm);

    // enable channel for slave mode:
    // - OUT endpoint: it will enable corresponding FIFO channel
    // - IN endpoint: it will write an IN request to the Non-periodic Request Queue, this will have dwc2 trying to send
    // IN Token. If we got NAK, we have to re-enable the channel again in the interrupt. Due to the way usbh stack only
    // call hcd_edpt_xfer() once, we will need to manage de-allocate/re-allocate IN channel dynamically.
    if (ep_dir == TUSB_DIR_IN) {
      TU_ASSERT(request_queue_avail(dwc2, is_period));
      channel->hcchar |= HCCHAR_CHENA;
    } else {
      channel->hcchar |= HCCHAR_CHENA;
      if (buflen > 0) {
        // To prevent conflict with other channel, we will enable periodic/non-periodic FIFO empty interrupt accordingly
        // And write packet in the interrupt handler
        dwc2->gintmsk |= (is_period ? GINTSTS_PTX_FIFO_EMPTY : GINTSTS_NPTX_FIFO_EMPTY);
      }
    }
  }

  channel->hcintmsk = hcintmsk;
  dwc2->haintmsk |= TU_BIT(ch_id);

  return true;
}

// Abort a queued transfer. Note: it can only abort transfer that has not been started
// Return true if a queued transfer is aborted, false if there is no transfer to abort
bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const uint8_t ep_num = tu_edpt_number(ep_addr);
  const uint8_t ep_dir = tu_edpt_dir(ep_addr);
  const uint8_t ep_id = edpt_find_opened(dev_addr, ep_num, ep_dir);
  TU_VERIFY(ep_id < CFG_TUH_DWC2_ENDPOINT_MAX);
  hcd_endpoint_t* edpt = &_hcd_data.edpt[ep_id];

  // hcd_int_disable(rhport);

  // Find enabled channeled and disable it, channel will be de-allocated in the interrupt handler
  const uint8_t ch_id = channel_find_enabled(dwc2, dev_addr, ep_num, ep_dir);
  if (ch_id < 16) {
    dwc2_channel_t* channel = &dwc2->channel[ch_id];
    // disable also require request queue
    if (request_queue_avail(dwc2, channel_is_periodic(edpt->hcchar_bm))) {
      channel->hcchar |= HCCHAR_CHDIS;
    } else {
      TU_BREAKPOINT();
    }
  }

  // hcd_int_enable(rhport);

  return true;
}

// Submit a special transfer to send 8-byte Setup Packet, when complete hcd_event_xfer_complete() must be invoked
bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, const uint8_t setup_packet[8]) {
  uint8_t ep_id = edpt_find_opened(dev_addr, 0, TUSB_DIR_OUT);
  TU_ASSERT(ep_id < CFG_TUH_DWC2_ENDPOINT_MAX); // no opened endpoint
  hcd_endpoint_t* edpt = &_hcd_data.edpt[ep_id];
  edpt->next_data_toggle = HCTSIZ_PID_SETUP;

  return hcd_edpt_xfer(rhport, dev_addr, 0, (uint8_t*)(uintptr_t) setup_packet, 8);
}

// clear stall, data toggle is also reset to DATA0
bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;

  return false;
}

//--------------------------------------------------------------------
// HCD Event Handler
//--------------------------------------------------------------------

static void handle_rxflvl_irq(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  // Pop control word off FIFO
  const dwc2_grxstsp_t grxstsp_bm = dwc2->grxstsp_bm;
  const uint8_t ch_id = grxstsp_bm.ep_ch_num;
  dwc2_channel_t* channel = &dwc2->channel[ch_id];

  switch (grxstsp_bm.packet_status) {
    case GRXSTS_PKTSTS_RX_DATA: {
      // In packet received
      const uint16_t byte_count = grxstsp_bm.byte_count;
      hcd_xfer_t* xfer = &_hcd_data.xfer[ch_id];

      dfifo_read_packet(dwc2, xfer->buffer, byte_count);
      xfer->buffer += byte_count;

      // short packet, minus remaining bytes (xfer_size)
      if (byte_count < channel->hctsiz_bm.xfer_size) {
        xfer->total_bytes -= channel->hctsiz_bm.xfer_size;
      }

      break;
    }

    case GRXSTS_PKTSTS_RX_COMPLETE:
      // In transfer complete: After this entry is popped from the receive FIFO, dwc2 asserts a Transfer Completed
      // interrupt --> handle_channel_irq()
      break;

    case GRXSTS_PKTSTS_HOST_DATATOGGLE_ERR:
      TU_ASSERT(0, ); // maybe try to change DToggle
      break;

    case GRXSTS_PKTSTS_HOST_CHANNEL_HALTED:
      // triggered when channel.hcchar_bm.disable is set
      // TODO handle later
      break;

    default: break; // ignore other status
  }
}

/* Handle Host Port interrupt, possible source are:
   - Connection Detection
   - Enable Change
   - Over Current Change
*/
TU_ATTR_ALWAYS_INLINE static inline void handle_hprt_irq(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK;
  const dwc2_hprt_t hprt_bm = dwc2->hprt_bm;

  if (dwc2->hprt & HPRT_CONN_DETECT) {
    // Port Connect Detect
    hprt |= HPRT_CONN_DETECT;

    if (hprt_bm.conn_status) {
      hcd_event_device_attach(rhport, in_isr);
    } else {
      hcd_event_device_remove(rhport, in_isr);
    }
  }

  if (dwc2->hprt & HPRT_ENABLE_CHANGE) {
    // Port enable change
    hprt |= HPRT_ENABLE_CHANGE;

    if (hprt_bm.enable) {
      // Port enable
      // Config HCFG FS/LS clock and HFIR for SOF interval according to link speed (value is in PHY clock unit)
      const tusb_speed_t speed = convert_hprt_speed(hprt_bm.speed);
      uint32_t hcfg = dwc2->hcfg & ~HCFG_FSLS_PHYCLK_SEL;

      const dwc2_gusbcfg_t gusbcfg_bm = dwc2->gusbcfg_bm;
      uint32_t clock = 60;
      if (gusbcfg_bm.phy_sel) {
        // dedicated FS is 48Mhz
        clock = 48;
        hcfg |= HCFG_FSLS_PHYCLK_SEL_48MHZ;
      } else {
        // UTMI+ or ULPI
        if (gusbcfg_bm.ulpi_utmi_sel) {
          clock = 60; // ULPI 8-bit is  60Mhz
        } else if (gusbcfg_bm.phy_if16) {
          clock = 30; // UTMI+ 16-bit is 30Mhz
        } else {
          clock = 60; // UTMI+ 8-bit is 60Mhz
        }
        hcfg |= HCFG_FSLS_PHYCLK_SEL_30_60MHZ;
      }

      dwc2->hcfg = hcfg;

      uint32_t hfir = dwc2->hfir & ~HFIR_FRIVL_Msk;
      if (speed == TUSB_SPEED_HIGH) {
        hfir |= 125*clock;
      } else {
        hfir |= 1000*clock;
      }

      dwc2->hfir = hfir;
    }
  }

  dwc2->hprt = hprt; // clear interrupt
}

// DMA related error: HCINT_BUFFER_NA  | HCINT_DESC_ROLLOVER
// if (hcint & (HCINT_BUFFER_NA  | HCINT_DESC_ROLLOVER)) {
//   result = XFER_RESULT_FAILED;
// }
void handle_channel_irq(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const bool is_dma = dma_host_enabled(dwc2);
  const uint8_t max_channel = DWC2_CHANNEL_COUNT(dwc2);

  for (uint8_t ch_id = 0; ch_id < max_channel; ch_id++) { //
    if (tu_bit_test(dwc2->haint, ch_id)) {
      dwc2_channel_t* channel = &dwc2->channel[ch_id];
      hcd_xfer_t* xfer = &_hcd_data.xfer[ch_id];

      uint32_t hcint = channel->hcint;
      hcint &= channel->hcintmsk;

      dwc2_channel_char_t hcchar_bm = channel->hcchar_bm;
      xfer_result_t result = XFER_RESULT_INVALID; // invalid means not DONE

      if (is_dma) {

      } else {
        if (hcint & HCINT_XFER_COMPLETE) {
          result = XFER_RESULT_SUCCESS;
          channel->hcintmsk &= ~HCINT_ACK;
          channel_dealloc(dwc2, ch_id);
        } else if (hcint & HCINT_STALL) {
          result = XFER_RESULT_STALLED;
          channel->hcintmsk |= HCINT_CHANNEL_HALTED;
          channel->hcchar_bm.disable = 1;
        } else if (hcint & (HCINT_NAK | HCINT_XACT_ERR | HCINT_NYET)) {
          if (hcchar_bm.ep_dir == TUSB_DIR_OUT) {
            // rewind buffer
          } else {
            if (hcint & HCINT_NAK) {
              // NAK received, re-enable channel if request queue is available
              TU_ASSERT(request_queue_avail(dwc2, channel_is_periodic(hcchar_bm)), );
              channel->hcchar |= HCCHAR_CHENA;
            }
          }

          // unmask halted
          // disable channel
          if (hcint & HCINT_XACT_ERR) {
            xfer->err_count++;
            channel->hcintmsk |= HCINT_ACK;
          }else {
            xfer->err_count = 0;
          }
        } else if (hcint & HCINT_CHANNEL_HALTED) {
          if (channel->hcchar_bm.err_multi_count == HCD_XFER_ERROR_MAX) {

          } else {
            // Re-initialize Channel (Do ping protocol for HS)
          }
        } else if (hcint & HCINT_ACK) {
          // ACK received, reset error count
          xfer->err_count = 0;
          channel->hcintmsk &= ~HCINT_ACK;
        }

        // const uint32_t xact_err = hcint & (HCINT_AHB_ERR | HCINT_XACT_ERR | HCINT_BABBLE_ERR |
        //                                    HCINT_DATATOGGLE_ERR | HCINT_XCS_XACT_ERR);
        // if (xact_err) {
        //   result = XFER_RESULT_FAILED;
        // }
        // if (!xact_err && (hcint & HCINT_CHANNEL_HALTED)) {
        //   // Channel halted without error, this is a response to channel disable
        //   result = XFER_RESULT_INVALID;
        // }


        // Transfer is complete (success, stalled, failed) or channel is disabled
        if (result != XFER_RESULT_INVALID || (hcint & HCINT_CHANNEL_HALTED)) {
          dwc2->haintmsk &= ~TU_BIT(ch_id); // de-allocate channel

          // notify usbh if transfer is complete (skip if channel is disabled)
          const uint8_t ep_addr = tu_edpt_addr(hcchar_bm.ep_num, hcchar_bm.ep_dir);
          if (result != XFER_RESULT_INVALID) {
            hcd_event_xfer_complete(hcchar_bm.dev_addr, ep_addr, 0, result, in_isr);
          }
        }
      }

      channel->hcint = hcint;  // clear all interrupt flags
    }
  }
}

// return true if there is still pending data and need more ISR
bool handle_txfifo_empty(dwc2_regs_t* dwc2, bool is_periodic) {
  // Use period txsts for both p/np to get request queue space available (1-bit difference, it is small enough)
  volatile dwc2_hptxsts_t* txsts_bm = (volatile dwc2_hptxsts_t*) (is_periodic ? &dwc2->hptxsts : &dwc2->hnptxsts);

  const uint8_t max_channel = DWC2_CHANNEL_COUNT(dwc2);
  for (uint8_t ch_id = 0; ch_id < max_channel; ch_id++) {
    hcd_xfer_t* xfer = &_hcd_data.xfer[ch_id];
    if (xfer->state == HCD_XFER_STATE_ACTIVE) {
      dwc2_channel_t* channel = &dwc2->channel[ch_id];
      const dwc2_channel_char_t hcchar_bm = channel->hcchar_bm;
      if (hcchar_bm.ep_dir == TUSB_DIR_OUT) {
        const uint16_t remain_packets = channel->hctsiz_bm.packet_count;
        for (uint16_t i = 0; i < remain_packets; i++) {
          const uint16_t remain_bytes = channel->hctsiz_bm.xfer_size;
          const uint16_t xact_bytes = tu_min16(remain_bytes, hcchar_bm.ep_size);

          // check if there is enough space in FIFO and RequestQueue.
          // Packet's last word written to FIFO will trigger a request queue
          if ((xact_bytes > txsts_bm->fifo_available) && (txsts_bm->req_queue_available > 0)) {
            return true;
          }

          dfifo_write_packet(dwc2, ch_id, xfer->buffer, xact_bytes);
          xfer->buffer += xact_bytes;
        }
      }
    }
  }

  return false; // all data written
}

/* Interrupt Hierarchy
               HCINTn         HPRT
                 |             |
               HAINT.CHn       |
                 |             |
    GINTSTS :  HCInt         PrtInt      NPTxFEmp PTxFEmpp RXFLVL


*/
void hcd_int_handler(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const uint32_t int_mask = dwc2->gintmsk;
  const uint32_t int_status = dwc2->gintsts & int_mask;

  // TU_LOG1_HEX(int_status);

  if (int_status & GINTSTS_CONIDSTSCHNG) {
    // Connector ID status change
    dwc2->gintsts = GINTSTS_CONIDSTSCHNG;

    //if (dwc2->gotgctl)
    // dwc2->hprt = HPRT_POWER; // power on port to turn on VBUS
    //dwc2->gintmsk |= GINTMSK_PRTIM;
    // TODO wait for SRP if OTG
  }

  if (int_status & GINTSTS_HPRTINT) {
    // Host port interrupt: source is cleared in HPRT register
    // TU_LOG1_HEX(dwc2->hprt);
    handle_hprt_irq(rhport, in_isr);
  }

  if (int_status & GINTSTS_HCINT) {
    // Host Channel interrupt: source is cleared in HCINT register
    handle_channel_irq(rhport, in_isr);
  }

  if (int_status & GINTSTS_NPTX_FIFO_EMPTY) {
    // NPTX FIFO empty interrupt, this is read-only and cleared by hardware when FIFO is written
    const bool more_isr = handle_txfifo_empty(dwc2, false);
    if (!more_isr) {
      // no more pending packet, disable interrupt
      dwc2->gintmsk &= ~GINTSTS_NPTX_FIFO_EMPTY;
    }
  }

  // RxFIFO non-empty interrupt handling.
  if (int_status & GINTSTS_RXFLVL) {
    // RXFLVL bit is read-only
    dwc2->gintmsk &= ~GINTMSK_RXFLVLM; // disable RXFLVL interrupt while reading

    do {
      handle_rxflvl_irq(rhport); // read all packets
    } while(dwc2->gintsts & GINTSTS_RXFLVL);

    dwc2->gintmsk |= GINTMSK_RXFLVLM;
  }

}

#endif
