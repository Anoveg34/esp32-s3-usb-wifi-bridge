#pragma once

#include <stdbool.h>
#include "tinyusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_NET_STRING_COUNT 6

/* nic=false: share-mode ICS gadget. nic=true: independent Wi-Fi NIC (different PID/serial). */
void usb_desc_fill(tinyusb_config_t *cfg, const uint8_t mac[6], bool nic);

#ifdef __cplusplus
}
#endif
