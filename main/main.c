/*
 * USB (RNDIS or NCM) WAN + Wi-Fi SoftAP NAT.
 * SoftAP 192.168.4.1, admin at http://192.168.4.1
 * ota_0 = RNDIS firmware, ota_1 = NCM firmware.
 */

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_boot.h"
#include "usb_wan.h"
#include "wifi_ap.h"
#include "nat.h"
#include "web_server.h"

static const char *TAG = "usb2ap";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    app_config_load();
    app_boot_sync_from_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(usb_wan_start());
    ESP_ERROR_CHECK(wifi_ap_start());
    ESP_ERROR_CHECK(net_nat_bind(usb_wan_netif(), wifi_ap_netif()));
    ESP_ERROR_CHECK(web_server_start());

    const app_cfg_t *cfg = app_config_get();
    ESP_LOGI(TAG, "USB=%s SSID:%s hidden:%d ch:%u admin http://192.168.4.1/",
             app_boot_firmware_is_ncm() ? "NCM" : "RNDIS",
             cfg->ssid, cfg->hidden, cfg->channel);
}
