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
#include "wifi_sta.h"
#include "usb_wan.h"
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
    char usb_lan_ip[16] = {0};
    if (cfg->sta_nic) {
        usb_net_admin_ip(admin_ip, sizeof(admin_ip));
        strlcpy(usb_lan_ip, admin_ip[0] ? admin_ip : USB_LAN_IP_STR, sizeof(usb_lan_ip));
    } else {
        wifi_ap_admin_ip(admin_ip, sizeof(admin_ip));
    }
    char sta_ip[16] = {0};
    if (cfg->sta_nic) {
        wifi_sta_ip(sta_ip, sizeof(sta_ip));
    }
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
    cJSON_AddBoolToObject(root, "sta_nic", cfg->sta_nic);
    cJSON_AddStringToObject(root, "sta_ssid", cfg->sta_ssid);
    cJSON_AddBoolToObject(root, "sta_open", cfg->sta_password[0] == 0);
    cJSON_AddBoolToObject(root, "sta_connected", cfg->sta_nic && wifi_sta_is_connected());
    cJSON_AddNumberToObject(root, "sta_reason", cfg->sta_nic ? wifi_sta_fail_reason() : 0);
    char sta_ssid_now[33] = {0};
    if (cfg->sta_nic) {
        wifi_sta_connected_ssid(sta_ssid_now, sizeof(sta_ssid_now));
    }
    cJSON_AddStringToObject(root, "sta_live_ssid", sta_ssid_now);
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddStringToObject(root, "admin_ip", admin_ip[0] ? admin_ip : (cfg->sta_nic ? USB_LAN_IP_STR : "192.168.4.1"));
    cJSON_AddStringToObject(root, "usb_lan_ip", cfg->sta_nic ? (usb_lan_ip[0] ? usb_lan_ip : USB_LAN_IP_STR) : "");
    /* mDNS only binds the predefined Wi-Fi netifs, so the name is unreachable
     * over USB. Report it empty in NIC mode and the UI hides the link. */
    cJSON_AddStringToObject(root, "hostname", cfg->sta_nic ? "" : APP_MDNS_HOSTNAME ".local");
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
    char body[1024];
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
    const cJSON *sta_nic = cJSON_GetObjectItemCaseSensitive(json, "sta_nic");
    bool mode_changed = false;
    if (cJSON_IsBool(sta_nic)) {
        bool want_nic = cJSON_IsTrue(sta_nic);
        mode_changed = (want_nic != cfg.sta_nic);
        cfg.sta_nic = want_nic;
    }
    cJSON_Delete(json);

    if (app_config_save(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    bool need_reboot = mode_changed;
    if (usb_changed && cfg.usb_ncm != app_boot_firmware_is_ncm()) {
        esp_err_t sw = app_boot_select_usb(cfg.usb_ncm);
        if (sw != ESP_OK) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddBoolToObject(root, "reboot", false);
            cJSON_AddStringToObject(root, "message",
                                    cfg.usb_ncm
                                        ? "已保存，但 Flash 里还没有 NCM 固件。请运行 python tools/dual_fw.py flash 把两套固件都烧进去。"
                                        : "已保存，但 Flash 里还没有 RNDIS 固件。请运行 python tools/dual_fw.py flash 把两套固件都烧进去。");
            return send_json(req, root);
        }
        need_reboot = true;
    }

    if (need_reboot) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "reboot", true);
        if (mode_changed && cfg.sta_nic) {
            cJSON_AddStringToObject(root, "message",
                                    "正在重启为 USB 网卡模式。板子热点已关掉。电脑请让 USB 网卡自动获取 IP（192.168.5.x），打开 http://192.168.5.1 再连家里 Wi-Fi。不要开 Windows 网络共享。");
        } else if (mode_changed) {
            cJSON_AddStringToObject(root, "message",
                                    "正在重启为共享热点模式。请用手机连板子热点，打开 http://192.168.4.1 。电脑需把有线网上网共享给 USB 网卡。");
        } else {
            cJSON_AddStringToObject(root, "message", "正在重启到另一套 USB 固件，请稍后重新打开管理页。");
        }
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
        return send_json(req, root);
    }

    if (wifi_ap_is_started()) {
        wifi_ap_apply_config();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reboot", false);
    cJSON_AddStringToObject(root, "message", "设置已生效。");
    return send_json(req, root);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    char query[32] = {0};
    char val[8] = {0};
    bool force = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "force", val, sizeof(val)) == ESP_OK &&
        val[0] == '1') {
        force = true;
    }
    wifi_scan_item_t items[WIFI_SCAN_MAX];
    int n = wifi_radio_scan(items, WIFI_SCAN_MAX, force);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");
    for (int i = 0; i < n; i++) {
        cJSON *one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(one, "rssi", items[i].rssi);
        cJSON_AddBoolToObject(one, "open", items[i].open);
        cJSON_AddNumberToObject(one, "channel", items[i].channel);
        cJSON_AddItemToArray(arr, one);
    }
    return send_json(req, root);
}

static esp_err_t handle_sta_connect(httpd_req_t *req)
{
    if (!app_config_get()->sta_nic) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not nic mode");
    }
    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    const cJSON *open = cJSON_GetObjectItemCaseSensitive(json, "open");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || ssid->valuestring[0] == 0) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
    }
    app_cfg_t cfg;
    app_config_get_copy(&cfg);
    strlcpy(cfg.sta_ssid, ssid->valuestring, sizeof(cfg.sta_ssid));
    if (cJSON_IsTrue(open)) {
        cfg.sta_password[0] = 0;
    } else if (cJSON_IsString(password) && password->valuestring && password->valuestring[0]) {
        strlcpy(cfg.sta_password, password->valuestring, sizeof(cfg.sta_password));
    } else if (cfg.sta_password[0] == 0) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password");
    }
    cJSON_Delete(json);
    if (app_config_save(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }
    if (wifi_sta_connect_now() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connect");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "正在连接家里的 Wi-Fi。");
    return send_json(req, root);
}

static esp_err_t handle_sta_disconnect(httpd_req_t *req)
{
    if (!app_config_get()->sta_nic) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not nic mode");
    }
    (void)wifi_sta_disconnect_now();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "已断开家里的 Wi-Fi。");
    return send_json(req, root);
}

static esp_err_t handle_stations(httpd_req_t *req)
{
    wifi_sta_list_t list = {0};
    if (wifi_ap_is_started()) {
        (void)esp_wifi_ap_get_sta_list(&list);
    }

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
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = handle_scan},
        {.uri = "/api/sta/connect", .method = HTTP_POST, .handler = handle_sta_connect},
        {.uri = "/api/sta/disconnect", .method = HTTP_POST, .handler = handle_sta_disconnect},
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
    if (app_config_get()->sta_nic) {
        ESP_LOGI(TAG, "admin UI http://%s/", USB_LAN_IP_STR);
    } else {
        ESP_LOGI(TAG, "admin UI http://192.168.4.1/  or http://%s.local/", APP_MDNS_HOSTNAME);
    }
    return ESP_OK;
}
