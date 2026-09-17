#pragma once

#include "tinyusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_NET_STRING_COUNT 6

void usb_desc_fill(tinyusb_config_t *cfg, const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
