#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t net_nat_bind(esp_netif_t *wan, esp_netif_t *ap);
