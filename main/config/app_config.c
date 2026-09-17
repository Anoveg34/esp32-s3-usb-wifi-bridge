#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "app_config.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "share";
static app_cfg_t s_cfg;
static bool s_loaded;

static void set_defaults(app_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->ssid, CONFIG_EXAMPLE_WIFI_SSID, sizeof(cfg->ssid));
    strlcpy(cfg->password, CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(cfg->password));
    cfg->channel = CONFIG_EXAMPLE_WIFI_CHANNEL;
    cfg->hidden = false;
#if CONFIG_EXAMPLE_USB_NET_MODE_NCM
    cfg->usb_ncm = true;
#else
    cfg->usb_ncm = false;
#endif
}

void app_config_load(void)
{
    set_defaults(&s_cfg);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        s_loaded = true;
        return;
    }
    size_t len = sizeof(s_cfg.ssid);
    nvs_get_str(nvs, "ssid", s_cfg.ssid, &len);
    len = sizeof(s_cfg.password);
    nvs_get_str(nvs, "pass", s_cfg.password, &len);
    uint8_t u8 = s_cfg.channel;
    if (nvs_get_u8(nvs, "ch", &u8) == ESP_OK && u8 >= 1 && u8 <= 13) {
        s_cfg.channel = u8;
    }
    if (nvs_get_u8(nvs, "hid", &u8) == ESP_OK) {
        s_cfg.hidden = u8 != 0;
    }
    if (nvs_get_u8(nvs, "ncm", &u8) == ESP_OK) {
        s_cfg.usb_ncm = u8 != 0;
    }
    len = sizeof(s_cfg.blocked);
    if (nvs_get_blob(nvs, "blk", s_cfg.blocked, &len) == ESP_OK) {
        s_cfg.block_count = (uint8_t)(len / 6);
        if (s_cfg.block_count > APP_CFG_BLOCK_MAX) {
            s_cfg.block_count = APP_CFG_BLOCK_MAX;
        }
    }
    nvs_close(nvs);
    s_loaded = true;
    ESP_LOGI(TAG, "loaded SSID=%s ch=%u hidden=%d ncm=%d blocks=%u",
             s_cfg.ssid, s_cfg.channel, s_cfg.hidden, s_cfg.usb_ncm, s_cfg.block_count);
}

esp_err_t app_config_save(const app_cfg_t *cfg)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open");
    (void)nvs_erase_key(nvs, "brg");
    esp_err_t err = nvs_set_str(nvs, "ssid", cfg->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "ch", cfg->channel);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "hid", cfg->hidden ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "ncm", cfg->usb_ncm ? 1 : 0);
    }
    if (err == ESP_OK) {
        if (cfg->block_count == 0) {
            err = nvs_erase_key(nvs, "blk");
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_blob(nvs, "blk", cfg->blocked, cfg->block_count * 6);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_cfg = *cfg;
    }
    return err;
}

const app_cfg_t *app_config_get(void)
{
    if (!s_loaded) {
        app_config_load();
    }
    return &s_cfg;
}

void app_config_get_copy(app_cfg_t *out)
{
    *out = *app_config_get();
}

bool app_config_is_blocked(const uint8_t mac[6])
{
    const app_cfg_t *cfg = app_config_get();
    for (int i = 0; i < cfg->block_count; i++) {
        if (memcmp(cfg->blocked[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_config_block_mac(const uint8_t mac[6], bool block)
{
    app_cfg_t cfg;
    app_config_get_copy(&cfg);
    int found = -1;
    for (int i = 0; i < cfg.block_count; i++) {
        if (memcmp(cfg.blocked[i], mac, 6) == 0) {
            found = i;
            break;
        }
    }
    if (block) {
        if (found >= 0) {
            return ESP_OK;
        }
        if (cfg.block_count >= APP_CFG_BLOCK_MAX) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(cfg.blocked[cfg.block_count], mac, 6);
        cfg.block_count++;
    } else if (found >= 0) {
        memmove(cfg.blocked[found], cfg.blocked[found + 1],
                (cfg.block_count - found - 1) * 6);
        cfg.block_count--;
    }
    return app_config_save(&cfg);
}
