#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "mdns.h"

#include "web_server.h"
#include "app_config.h"
#include "app_boot.h"
#include "wifi_ap.h"
#include "sdkconfig.h"

static const char *TAG = "web";
#define APP_MDNS_HOSTNAME "esp32-share"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    if (s == NULL) {
        return false;
    }
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        sscanf(s, "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 255) {
            return false;
        }
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static void mac_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    int off = 0;
    while (off < total) {
        int n = httpd_req_recv(req, buf + off, total - off);
        if (n <= 0) {
            return ESP_FAIL;
        }
        off += n;
    }
    buf[off] = 0;
    return ESP_OK;
}

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    const app_cfg_t *cfg = app_config_get();
    char admin_ip[16] = {0};
    wifi_ap_admin_ip(admin_ip, sizeof(admin_ip));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", cfg->ssid);
    cJSON_AddNumberToObject(root, "channel", cfg->channel);
    cJSON_AddBoolToObject(root, "hidden", cfg->hidden);
    cJSON_AddBoolToObject(root, "open", cfg->password[0] == 0);
    cJSON_AddBoolToObject(root, "usb_ncm", cfg->usb_ncm);
    cJSON_AddBoolToObject(root, "firmware_ncm", app_boot_firmware_is_ncm());
    cJSON_AddStringToObject(root, "firmware_usb", app_boot_firmware_is_ncm() ? "NCM" : "RNDIS");
    cJSON_AddBoolToObject(root, "rndis_ready", app_boot_slot_ready(false));
    cJSON_AddBoolToObject(root, "ncm_ready", app_boot_slot_ready(true));
    cJSON_AddStringToObject(root, "admin_ip", admin_ip[0] ? admin_ip : "192.168.4.1");
    cJSON_AddStringToObject(root, "hostname", APP_MDNS_HOSTNAME ".local");
    return send_json(req, root);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }

    app_cfg_t cfg;
    app_config_get_copy(&cfg);

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    if (cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0]) {
        strlcpy(cfg.ssid, ssid->valuestring, sizeof(cfg.ssid));
    }
    const cJSON *open = cJSON_GetObjectItemCaseSensitive(json, "open");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_IsTrue(open)) {
        cfg.password[0] = 0;
    } else if (cJSON_IsString(password) && password->valuestring && password->valuestring[0]) {
        if (strlen(password->valuestring) < 8) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too short");
        }
        strlcpy(cfg.password, password->valuestring, sizeof(cfg.password));
    }
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(json, "channel");
    if (cJSON_IsNumber(channel)) {
        int ch = channel->valueint;
        if (ch < 1 || ch > 13) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel");
        }
        cfg.channel = (uint8_t)ch;
    }
    const cJSON *hidden = cJSON_GetObjectItemCaseSensitive(json, "hidden");
    if (cJSON_IsBool(hidden)) {
        cfg.hidden = cJSON_IsTrue(hidden);
    }
    const cJSON *usb_ncm = cJSON_GetObjectItemCaseSensitive(json, "usb_ncm");
    bool usb_changed = false;
    if (cJSON_IsBool(usb_ncm)) {
        bool want_ncm = cJSON_IsTrue(usb_ncm);
        usb_changed = (want_ncm != cfg.usb_ncm);
        cfg.usb_ncm = want_ncm;
    }
    cJSON_Delete(json);

    if (app_config_save(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }
    wifi_ap_apply_config();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);

    if (usb_changed && cfg.usb_ncm != app_boot_firmware_is_ncm()) {
        esp_err_t sw = app_boot_select_usb(cfg.usb_ncm);
        if (sw != ESP_OK) {
            cJSON_AddBoolToObject(root, "reboot", false);
            cJSON_AddStringToObject(root, "message",
                                    cfg.usb_ncm
                                        ? "已保存，但 Flash 里还没有 NCM 固件。请运行 python tools/dual_fw.py flash 把两套固件都烧进去。"
                                        : "已保存，但 Flash 里还没有 RNDIS 固件。请运行 python tools/dual_fw.py flash 把两套固件都烧进去。");
            return send_json(req, root);
        }
        cJSON_AddBoolToObject(root, "reboot", true);
        cJSON_AddStringToObject(root, "message", "正在重启到另一套 USB 固件，请稍后重新连接热点。");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
        return send_json(req, root);
    }

    cJSON_AddBoolToObject(root, "reboot", false);
    cJSON_AddStringToObject(root, "message", "热点设置已生效。");
    return send_json(req, root);
}

static esp_err_t handle_stations(httpd_req_t *req)
{
    wifi_sta_list_t list = {0};
    esp_wifi_ap_get_sta_list(&list);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "stations");
    for (int i = 0; i < list.num; i++) {
        char mac[18];
        mac_str(list.sta[i].mac, mac);
        cJSON *one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "mac", mac);
        uint32_t ip = wifi_ap_lookup_client_ip(list.sta[i].mac);
        if (ip) {
            char ipbuf[16];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR((esp_ip4_addr_t *)&ip));
            cJSON_AddStringToObject(one, "ip", ipbuf);
        } else {
            cJSON_AddStringToObject(one, "ip", "");
        }
        cJSON_AddNumberToObject(one, "rssi", list.sta[i].rssi);
        cJSON_AddItemToArray(arr, one);
    }

    const app_cfg_t *cfg = app_config_get();
    cJSON *blocked = cJSON_AddArrayToObject(root, "blocked");
    for (int i = 0; i < cfg->block_count; i++) {
        char mac[18];
        mac_str(cfg->blocked[i], mac);
        cJSON_AddItemToArray(blocked, cJSON_CreateString(mac));
    }
    return send_json(req, root);
}

static esp_err_t handle_kick(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    const cJSON *macj = cJSON_GetObjectItemCaseSensitive(json, "mac");
    uint8_t mac[6];
    bool ok = cJSON_IsString(macj) && parse_mac(macj->valuestring, mac);
    cJSON_Delete(json);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mac");
    }
    wifi_ap_kick_mac(mac);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t handle_block(httpd_req_t *req)
{
    char body[160];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    const cJSON *macj = cJSON_GetObjectItemCaseSensitive(json, "mac");
    const cJSON *blockj = cJSON_GetObjectItemCaseSensitive(json, "block");
    uint8_t mac[6];
    bool ok = cJSON_IsString(macj) && parse_mac(macj->valuestring, mac);
    bool do_block = !cJSON_IsFalse(blockj);
    cJSON_Delete(json);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mac");
    }
    if (app_config_block_mac(mac, do_block) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "block list full");
    }
    if (do_block) {
        wifi_ap_kick_mac(mac);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config},
        {.uri = "/api/stations", .method = HTTP_GET, .handler = handle_stations},
        {.uri = "/api/kick", .method = HTTP_POST, .handler = handle_kick},
        {.uri = "/api/block", .method = HTTP_POST, .handler = handle_block},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(APP_MDNS_HOSTNAME));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_instance_name_set("ESP32-S3 Share"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "admin UI http://192.168.4.1/  or http://%s.local/", APP_MDNS_HOSTNAME);
    return ESP_OK;
}
