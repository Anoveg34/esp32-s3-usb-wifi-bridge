/*
 * Two exclusive work modes:
 *   share: USB WAN (Windows ICS DHCP client) + SoftAP NAT
 *   nic:   Wi-Fi STA WAN + USB LAN DHCP, no SoftAP, no ICS
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
#include "wifi_sta.h"
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

    const app_cfg_t *cfg = app_config_get();
    if (cfg->sta_nic) {
        ESP_ERROR_CHECK(usb_lan_start());
        ESP_ERROR_CHECK(wifi_sta_start());
        ESP_ERROR_CHECK(net_nat_bind(wifi_sta_netif(), usb_net_netif()));
        ESP_LOGI(TAG, "mode=USB-NIC USB=%s STA SSID:%s admin http://%s/",
                 app_boot_firmware_is_ncm() ? "NCM" : "RNDIS",
                 cfg->sta_ssid[0] ? cfg->sta_ssid : "(unset)",
                 USB_LAN_IP_STR);
    } else {
        ESP_ERROR_CHECK(usb_wan_start());
        ESP_ERROR_CHECK(wifi_ap_start());
        ESP_ERROR_CHECK(net_nat_bind(usb_net_netif(), wifi_ap_netif()));
        ESP_LOGI(TAG, "mode=share USB=%s AP SSID:%s hidden:%d ch:%u admin http://192.168.4.1/",
                 app_boot_firmware_is_ncm() ? "NCM" : "RNDIS",
                 cfg->ssid, cfg->hidden, cfg->channel);
    }
    ESP_ERROR_CHECK(web_server_start());
}
