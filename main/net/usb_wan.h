#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t usb_wan_start(void);
esp_netif_t *usb_wan_netif(void);
