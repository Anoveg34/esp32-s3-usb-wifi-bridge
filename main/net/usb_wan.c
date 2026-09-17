#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tusb.h"
#include "class/net/net_device.h"
#include "usb_wan.h"
#include "usb_desc.h"
#include "app_boot.h"
#include "sdkconfig.h"

static const char *TAG = "usb_wan";
static esp_netif_t *s_usb_netif;
uint8_t tud_network_mac_address[6];

static void usb_rx_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGD(TAG, "USB TX failed");
    }
    return ESP_OK;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_usb_netif == NULL || len == 0) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    if (esp_netif_receive(s_usb_netif, copy, len, NULL) != ESP_OK) {
        free(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void usb_set_connected(bool up)
{
    if (s_usb_netif == NULL) {
        return;
    }
    tud_network_link_state(0, up);
    if (up) {
        esp_netif_action_connected(s_usb_netif, 0, 0, 0);
    } else {
        esp_netif_action_disconnected(s_usb_netif, 0, 0, 0);
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB mounted");
        usb_set_connected(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, "USB unmounted");
        usb_set_connected(false);
    }
}

esp_netif_t *usb_wan_netif(void)
{
    return s_usb_netif;
}

esp_err_t usb_wan_start(void)
{
    uint8_t usb_mac[6];
    uint8_t lwip_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(usb_mac, ESP_MAC_WIFI_STA), TAG, "read MAC");
    memcpy(lwip_mac, usb_mac, 6);
    lwip_mac[0] |= 0x02;
    lwip_mac[5] ^= 0x55;
    memcpy(tud_network_mac_address, usb_mac, 6);

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "USB_WAN";
    base_cfg.if_desc = "usb wan";
    base_cfg.route_prio = 100;

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = usb_netif_transmit,
        .driver_free_rx_buffer = usb_rx_free,
    };
    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_usb_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_usb_netif, ESP_FAIL, TAG, "USB netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, lwip_mac), TAG, "set USB MAC");
    esp_netif_action_start(s_usb_netif, 0, 0, 0);

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    usb_desc_fill(&tusb_cfg, usb_mac);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install");

    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
    };
    memcpy(net_config.mac_addr, usb_mac, 6);
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_config), TAG, "USB net init");
    if (tud_mounted()) {
        usb_set_connected(true);
    }

    ESP_LOGI(TAG, "USB %s host MAC " MACSTR " lwIP MAC " MACSTR,
             app_boot_firmware_is_ncm() ? "NCM" : "RNDIS",
             MAC2STR(usb_mac), MAC2STR(lwip_mac));
    return ESP_OK;
}
