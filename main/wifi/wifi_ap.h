#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t wifi_ap_start(void);
esp_err_t wifi_ap_apply_config(void);
bool wifi_ap_is_started(void);
esp_netif_t *wifi_ap_netif(void);
void wifi_ap_admin_ip(char *buf, size_t len);
void wifi_ap_note_client_ip(const uint8_t mac[6], uint32_t ip);
uint32_t wifi_ap_lookup_client_ip(const uint8_t mac[6]);
esp_err_t wifi_ap_kick_mac(const uint8_t mac[6]);
