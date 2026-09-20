#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_sta.h"
#include "app_config.h"

static const char *TAG = "wifi_sta";
static esp_netif_t *s_sta_netif;
static bool s_connected;
static bool s_wifi_inited;
static bool s_started;
static bool s_auto_reconnect;
static uint8_t s_fail_reason;
static int s_fail_count;
static TickType_t s_next_retry;
static bool s_scan_busy;
static TickType_t s_last_scan;

static bool auth_is_open(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_OPEN || mode == WIFI_AUTH_OWE;
}

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_auto_reconnect && app_config_get()->sta_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = event_data;
        s_connected = false;
        s_fail_reason = e->reason;
        ESP_LOGW(TAG, "STA disconnected reason=%u", e->reason);
        if (!s_auto_reconnect || app_config_get()->sta_ssid[0] == 0) {
            return;
        }
        s_fail_count++;
        if (s_fail_count >= 8) {
            ESP_LOGE(TAG, "STA retry stopped after %d failures", s_fail_count);
            s_auto_reconnect = false;
            return;
        }
        TickType_t now = xTaskGetTickCount();
        if (now < s_next_retry) {
            return;
        }
        s_next_retry = now + pdMS_TO_TICKS(3000);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        s_connected = true;
        s_fail_count = 0;
        s_fail_reason = 0;
        ESP_LOGI(TAG, "STA IP " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

esp_err_t wifi_sta_apply_config(void)
{
    const app_cfg_t *cfg = app_config_get();
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, cfg->sta_ssid, sizeof(wifi_config.sta.ssid));
    if (!cfg->sta_password[0]) {
        wifi_config.sta.password[0] = 0;
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        strlcpy((char *)wifi_config.sta.password, cfg->sta_password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set STA");
    return ESP_OK;
}

esp_err_t wifi_sta_connect_now(void)
{
    s_auto_reconnect = true;
    s_fail_count = 0;
    s_fail_reason = 0;
    s_next_retry = 0;
    ESP_RETURN_ON_ERROR(wifi_sta_apply_config(), TAG, "apply STA");
    if (!s_started) {
        return ESP_OK;
    }
    const app_cfg_t *cfg = app_config_get();
    if (cfg->sta_ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "STA connect SSID=%s open=%d", cfg->sta_ssid, cfg->sta_password[0] == 0);
    (void)esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_sta_disconnect_now(void)
{
    s_auto_reconnect = false;
    s_connected = false;
    s_fail_count = 0;
    ESP_LOGI(TAG, "STA disconnect");
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return ESP_OK;
    }
    return err;
}

esp_netif_t *wifi_sta_netif(void)
{
    return s_sta_netif;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

uint8_t wifi_sta_fail_reason(void)
{
    return s_fail_reason;
}

void wifi_sta_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = 0;
    if (s_sta_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

void wifi_sta_connected_ssid(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = 0;
    wifi_ap_record_t ap = {0};
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) {
        strlcpy(buf, (char *)ap.ssid, len);
        return;
    }
    strlcpy(buf, app_config_get()->sta_ssid, len);
}

int wifi_radio_scan(wifi_scan_item_t *out, int max, bool force)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    if (!force && s_connected) {
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    if (!force && s_last_scan != 0 && (now - s_last_scan) < pdMS_TO_TICKS(15000)) {
        return 0;
    }
    if (s_scan_busy) {
        return 0;
    }
    s_scan_busy = true;
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    bool restore_ap = (mode == WIFI_MODE_AP);
    if (restore_ap) {
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            s_scan_busy = false;
            return 0;
        }
    }
    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 30;
    scan.scan_time.active.max = 60;
    scan.home_chan_dwell_time = 80;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        if (restore_ap) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scan_busy = false;
        return 0;
    }
    uint16_t n = (uint16_t)max;
    wifi_ap_record_t *rec = calloc((size_t)max, sizeof(*rec));
    if (rec == NULL) {
        if (restore_ap) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scan_busy = false;
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
        free(rec);
        if (restore_ap) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scan_busy = false;
        return 0;
    }
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (rec[i].ssid[0] == 0) {
            continue;
        }
        int exist = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, (char *)rec[i].ssid) == 0) {
                exist = j;
                break;
            }
        }
        if (exist >= 0) {
            if (rec[i].rssi > out[exist].rssi) {
                out[exist].rssi = rec[i].rssi;
                out[exist].channel = rec[i].primary;
                out[exist].open = auth_is_open(rec[i].authmode);
            }
            continue;
        }
        strlcpy(out[count].ssid, (char *)rec[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = rec[i].rssi;
        out[count].channel = rec[i].primary;
        out[count].open = auth_is_open(rec[i].authmode);
        count++;
    }
    free(rec);
    if (restore_ap) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    s_last_scan = xTaskGetTickCount();
    s_scan_busy = false;
    return count;
}

static esp_err_t sta_register_events(void)
{
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   sta_event_handler, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   sta_event_handler, NULL), TAG, "ip evt");
    return ESP_OK;
}

esp_err_t wifi_sta_start(void)
{
    ESP_RETURN_ON_ERROR(sta_register_events(), TAG, "evt");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "STA netif");

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
        ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable PS");
        s_wifi_inited = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    s_auto_reconnect = app_config_get()->sta_ssid[0] != 0;
    ESP_RETURN_ON_ERROR(wifi_sta_apply_config(), TAG, "apply STA");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_started = true;
    return ESP_OK;
}

esp_err_t wifi_sta_start_apsta(void)
{
    ESP_RETURN_ON_ERROR(sta_register_events(), TAG, "evt");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "STA netif");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA");
    s_wifi_inited = true;
    s_started = true;
    s_auto_reconnect = app_config_get()->sta_ssid[0] != 0;
    ESP_RETURN_ON_ERROR(wifi_sta_apply_config(), TAG, "apply STA");
    if (s_auto_reconnect) {
        (void)esp_wifi_connect();
    }
    return ESP_OK;
}
