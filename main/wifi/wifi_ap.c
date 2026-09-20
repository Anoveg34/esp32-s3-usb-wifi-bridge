#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "wifi_ap.h"
#include "app_config.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_ap";
#define STA_IP_MAX 16

typedef struct {
    uint8_t mac[6];
    uint32_t ip;
    bool used;
} sta_ip_entry_t;

static esp_netif_t *s_ap_netif;
static sta_ip_entry_t s_sta_ip[STA_IP_MAX];

void wifi_ap_note_client_ip(const uint8_t mac[6], uint32_t ip)
{
    int empty = -1;
    for (int i = 0; i < STA_IP_MAX; i++) {
        if (s_sta_ip[i].used && memcmp(s_sta_ip[i].mac, mac, 6) == 0) {
            s_sta_ip[i].ip = ip;
            return;
        }
        if (!s_sta_ip[i].used && empty < 0) {
            empty = i;
        }
    }
    if (empty >= 0) {
        memcpy(s_sta_ip[empty].mac, mac, 6);
        s_sta_ip[empty].ip = ip;
        s_sta_ip[empty].used = true;
    }
}

uint32_t wifi_ap_lookup_client_ip(const uint8_t mac[6])
{
    for (int i = 0; i < STA_IP_MAX; i++) {
        if (s_sta_ip[i].used && memcmp(s_sta_ip[i].mac, mac, 6) == 0) {
            return s_sta_ip[i].ip;
        }
    }
    return 0;
}

esp_err_t wifi_ap_kick_mac(const uint8_t mac[6])
{
    uint16_t aid = 0;
    if (esp_wifi_ap_get_sta_aid(mac, &aid) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_wifi_deauth_sta(aid);
}

esp_err_t wifi_ap_apply_config(void)
{
    const app_cfg_t *cfg = app_config_get();
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, cfg->ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(cfg->ssid);
    wifi_config.ap.channel = cfg->channel;
    wifi_config.ap.max_connection = CONFIG_EXAMPLE_MAX_STA_CONN;
    wifi_config.ap.ssid_hidden = cfg->hidden ? 1 : 0;
    wifi_config.ap.pmf_cfg.required = false;
    if (strlen(cfg->password) >= 8) {
        strlcpy((char *)wifi_config.ap.password, cfg->password, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_ap_record_t sta_ap = {0};
    if (esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK && sta_ap.primary >= 1) {
        wifi_config.ap.channel = sta_ap.primary;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set AP config");
    ESP_LOGI(TAG, "AP SSID=%s ch=%u hidden=%d auth=%s",
             wifi_config.ap.ssid, wifi_config.ap.channel, wifi_config.ap.ssid_hidden,
             wifi_config.ap.authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2");
    return ESP_OK;
}

esp_netif_t *wifi_ap_netif(void)
{
    return s_ap_netif;
}

bool wifi_ap_is_started(void)
{
    return s_ap_netif != NULL;
}

void wifi_ap_admin_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = 0;
    if (s_ap_netif == NULL) {
        strlcpy(buf, "192.168.4.1", len);
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        strlcpy(buf, "192.168.4.1", len);
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "STA " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
        if (app_config_is_blocked(event->mac)) {
            ESP_LOGW(TAG, "blocked STA " MACSTR ", deauth", MAC2STR(event->mac));
            esp_wifi_deauth_sta(event->aid);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "STA " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = event_data;
        ESP_LOGI(TAG, "DHCP " MACSTR " -> " IPSTR, MAC2STR(e->mac), IP2STR(&e->ip));
        wifi_ap_note_client_ip(e->mac, e->ip.addr);
    }
}

esp_err_t wifi_ap_start(void)
{
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                   wifi_event_handler, NULL), TAG, "dhcp evt");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "AP netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable PS");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(wifi_ap_apply_config(), TAG, "apply AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    return ESP_OK;
}
