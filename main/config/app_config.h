#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CFG_SSID_MAX      32
#define APP_CFG_PASS_MAX      64
#define APP_CFG_BLOCK_MAX     16

typedef struct {
    char ssid[APP_CFG_SSID_MAX + 1];
    char password[APP_CFG_PASS_MAX + 1];
    uint8_t channel;
    bool hidden;
    bool usb_ncm;
    bool sta_nic;
    char sta_ssid[APP_CFG_SSID_MAX + 1];
    char sta_password[APP_CFG_PASS_MAX + 1];
    uint8_t block_count;
    uint8_t blocked[APP_CFG_BLOCK_MAX][6];
} app_cfg_t;

void app_config_load(void);
esp_err_t app_config_save(const app_cfg_t *cfg);
const app_cfg_t *app_config_get(void);
void app_config_get_copy(app_cfg_t *out);

bool app_config_is_blocked(const uint8_t mac[6]);
esp_err_t app_config_block_mac(const uint8_t mac[6], bool block);

#ifdef __cplusplus
}
#endif
