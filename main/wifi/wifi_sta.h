#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#define WIFI_SCAN_MAX 24

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open;
} wifi_scan_item_t;

esp_err_t wifi_sta_start(void);
esp_err_t wifi_sta_start_apsta(void);
esp_err_t wifi_sta_apply_config(void);
esp_err_t wifi_sta_connect_now(void);
esp_err_t wifi_sta_disconnect_now(void);
esp_netif_t *wifi_sta_netif(void);
bool wifi_sta_is_connected(void);
uint8_t wifi_sta_fail_reason(void);
void wifi_sta_ip(char *buf, size_t len);
void wifi_sta_connected_ssid(char *buf, size_t len);
int wifi_radio_scan(wifi_scan_item_t *out, int max, bool force);
