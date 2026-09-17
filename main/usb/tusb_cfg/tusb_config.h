#pragma once

/*
 * Force TinyUSB class from Example Configuration. Keep TinyUSB Kconfig on
 * ECM/RNDIS so esp_tinyusb does not compile its own tinyusb_net.c
 * (we provide usb/net_class.c).
 */
#include_next "tusb_config.h"

#undef CFG_TUD_ECM_RNDIS
#undef CFG_TUD_NCM

#if CONFIG_EXAMPLE_USB_NET_MODE_NCM
#define CFG_TUD_ECM_RNDIS  0
#define CFG_TUD_NCM        1

#undef CFG_TUD_NCM_OUT_NTB_N
#undef CFG_TUD_NCM_IN_NTB_N
#undef CFG_TUD_NCM_OUT_NTB_MAX_SIZE
#undef CFG_TUD_NCM_IN_NTB_MAX_SIZE
#define CFG_TUD_NCM_OUT_NTB_N         3
#define CFG_TUD_NCM_IN_NTB_N          3
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  3200
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   3200
#else
#define CFG_TUD_ECM_RNDIS  1
#define CFG_TUD_NCM        0
#endif
