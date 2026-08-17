/*
 * TinyUSB Configuration
 * HID + CDC only
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// Core
#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_DEBUG              0

// Device
#define CFG_TUD_ENDPOINT0_SIZE      64

// CDC (Serial)
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_EP_BUFSIZE      64
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256

// HID (NKRO Keyboard only)
#define CFG_TUD_HID                 1
#define CFG_TUD_HID_EP_BUFSIZE      64

// Disable other classes
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_DFU                 0
#define CFG_TUD_DFU_RUNTIME         0
#define CFG_TUD_NET                 0
#define CFG_TUD_ECM_RNDIS           0
#define CFG_TUD_NCM                 0

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */