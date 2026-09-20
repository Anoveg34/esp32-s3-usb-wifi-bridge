#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#define USB_LAN_IP_STR "192.168.5.1"

/* Share mode: USB is a DHCP client toward Windows ICS. */
esp_err_t usb_wan_start(void);
/* NIC mode: USB is a DHCP LAN for the PC. Independent of ICS / SoftAP. */
esp_err_t usb_lan_start(void);

esp_netif_t *usb_net_netif(void);
void usb_net_admin_ip(char *buf, size_t len);
