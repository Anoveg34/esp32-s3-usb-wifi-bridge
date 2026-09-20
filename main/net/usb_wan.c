#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
static bool s_lan;
uint8_t tud_network_mac_address[6];
#define DHCPS_OFFER_DNS 0x02
#define USB_FRAME_MAX 1518
#define USB_TX_DEPTH 8
#define USB_RX_DEPTH 8

typedef struct {
    uint16_t len;
    uint8_t data[USB_FRAME_MAX];
} usb_frame_t;

static usb_frame_t s_tx_frames[USB_TX_DEPTH];
static usb_frame_t s_rx_frames[USB_RX_DEPTH];
static QueueHandle_t s_tx_free;
static QueueHandle_t s_tx_ready;
static QueueHandle_t s_rx_free;
static QueueHandle_t s_rx_ready;

static const esp_netif_ip_info_t s_usb_lan_ip = {
    .ip = { .addr = ESP_IP4TOADDR(192, 168, 5, 1) },
    .gw = { .addr = ESP_IP4TOADDR(192, 168, 5, 1) },
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

static void usb_rx_free(void *h, void *buffer)
{
    (void)h;
    if (buffer == NULL) {
        return;
    }
    usb_frame_t *frame = (usb_frame_t *)((uint8_t *)buffer - offsetof(usb_frame_t, data));
    if (frame < s_rx_frames || frame >= s_rx_frames + USB_RX_DEPTH) {
        return;
    }
    (void)xQueueSend(s_rx_free, &frame, 0);
}

static void usb_rx_task(void *arg)
{
    (void)arg;
    usb_frame_t *frame;
    for (;;) {
        if (xQueueReceive(s_rx_ready, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_usb_netif == NULL ||
            esp_netif_receive(s_usb_netif, frame->data, frame->len, frame) != ESP_OK) {
            usb_rx_free(NULL, frame->data);
        }
    }
}

static void usb_tx_task(void *arg)
{
    (void)arg;
    usb_frame_t *frame;
    for (;;) {
        if (xQueueReceive(s_tx_ready, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* One attempt only. A retry would queue another usbd_defer_func while the
         * previous one may still be pending, and the TinyUSB event queue is shared
         * with bus reset, control transfers, and RNDIS keepalives. */
        if (tud_mounted()) {
            (void)tinyusb_net_send_sync(frame->data, frame->len, NULL, pdMS_TO_TICKS(20));
        }
        (void)xQueueSend(s_tx_free, &frame, 0);
    }
}

static esp_err_t usb_frame_queues_init(void)
{
    s_tx_free = xQueueCreate(USB_TX_DEPTH, sizeof(usb_frame_t *));
    s_tx_ready = xQueueCreate(USB_TX_DEPTH, sizeof(usb_frame_t *));
    s_rx_free = xQueueCreate(USB_RX_DEPTH, sizeof(usb_frame_t *));
    s_rx_ready = xQueueCreate(USB_RX_DEPTH, sizeof(usb_frame_t *));
    ESP_RETURN_ON_FALSE(s_tx_free && s_tx_ready && s_rx_free && s_rx_ready,
                        ESP_ERR_NO_MEM, TAG, "USB frame queues");
    for (int i = 0; i < USB_TX_DEPTH; i++) {
        usb_frame_t *frame = &s_tx_frames[i];
        ESP_RETURN_ON_FALSE(xQueueSend(s_tx_free, &frame, 0) == pdTRUE, ESP_FAIL, TAG, "USB TX pool");
    }
    for (int i = 0; i < USB_RX_DEPTH; i++) {
        usb_frame_t *frame = &s_rx_frames[i];
        ESP_RETURN_ON_FALSE(xQueueSend(s_rx_free, &frame, 0) == pdTRUE, ESP_FAIL, TAG, "USB RX pool");
    }
    BaseType_t ok = xTaskCreatePinnedToCore(usb_tx_task, "usb_tx", 3072, NULL, 10, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "USB TX task");
    /* lwIP lives on CPU0. Keep RX off the TinyUSB core so RNDIS EP0 keepalives stay on time. */
    ok = xTaskCreatePinnedToCore(usb_rx_task, "usb_rx", 3072, NULL, 12, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "USB RX task");
    return ESP_OK;
}

static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (buffer == NULL || len == 0 || len > USB_FRAME_MAX || !tud_mounted()) {
        return ESP_OK;
    }
    usb_frame_t *frame = NULL;
    if (xQueueReceive(s_tx_free, &frame, 0) != pdTRUE) {
        return ESP_OK;
    }
    memcpy(frame->data, buffer, len);
    frame->len = (uint16_t)len;
    if (xQueueSend(s_tx_ready, &frame, 0) != pdTRUE) {
        (void)xQueueSend(s_tx_free, &frame, 0);
    }
    return ESP_OK;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_usb_netif == NULL || buffer == NULL || len == 0 || len > USB_FRAME_MAX) {
        return ESP_OK;
    }
    /* Copy only. malloc / esp_netif_receive must not run in the TinyUSB task:
     * RNDIS keepalives share that task with EP0 and the interrupt endpoint. */
    usb_frame_t *frame = NULL;
    if (xQueueReceive(s_rx_free, &frame, 0) != pdTRUE) {
        return ESP_OK;
    }
    memcpy(frame->data, buffer, len);
    frame->len = len;
    if (xQueueSend(s_rx_ready, &frame, 0) != pdTRUE) {
        (void)xQueueSend(s_rx_free, &frame, 0);
    }
    return ESP_OK;
}

static void usb_set_connected(bool up)
{
    if (s_usb_netif == NULL) {
        return;
    }
    if (up) {
        tud_network_link_state(0, true);
    }
    if (s_lan) {
        return;
    }
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
        ESP_LOGD(TAG, "USB mounted");
        usb_set_connected(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGD(TAG, "USB unmounted");
        usb_set_connected(false);
    }
}

static esp_err_t usb_start_stack(const uint8_t usb_mac[6])
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    tusb_cfg.task.priority = 22;
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.xCoreID = 1;
    tusb_cfg.pm_lock_enable = true;
    usb_desc_fill(&tusb_cfg, usb_mac, s_lan);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install");
    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
    };
    memcpy(net_config.mac_addr, usb_mac, 6);
    return tinyusb_net_init(&net_config);
}

esp_netif_t *usb_net_netif(void)
{
    return s_usb_netif;
}

void usb_net_admin_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = 0;
    if (s_usb_netif == NULL) {
        strlcpy(buf, s_lan ? USB_LAN_IP_STR : "192.168.4.1", len);
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_usb_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        strlcpy(buf, s_lan ? USB_LAN_IP_STR : "192.168.4.1", len);
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

static esp_err_t usb_net_start(bool lan)
{
    uint8_t usb_mac[6];
    uint8_t lwip_mac[6];
    s_lan = lan;
    ESP_RETURN_ON_ERROR(esp_read_mac(usb_mac, ESP_MAC_WIFI_STA), TAG, "read MAC");
    memcpy(lwip_mac, usb_mac, 6);
    lwip_mac[0] |= 0x02;
    lwip_mac[5] ^= 0x55;
    memcpy(tud_network_mac_address, usb_mac, 6);
    ESP_RETURN_ON_ERROR(usb_frame_queues_init(), TAG, "USB frame queues");

    if (lan) {
        ESP_RETURN_ON_ERROR(usb_start_stack(usb_mac), TAG, "USB net init");
    }

    esp_netif_inherent_config_t base_cfg;
    if (lan) {
        base_cfg = (esp_netif_inherent_config_t) {
            .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
            .ip_info = &s_usb_lan_ip,
            .if_key = "USB_LAN",
            .if_desc = "usb lan",
            .route_prio = 10,
        };
    } else {
        base_cfg = (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_ETH();
        base_cfg.if_key = "USB_WAN";
        base_cfg.if_desc = "usb wan";
        base_cfg.route_prio = 100;
    }

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

    if (lan) {
        uint32_t lease = 1440;
        uint8_t offer = DHCPS_OFFER_DNS;
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET,
                                                             ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                                             &lease, sizeof(lease)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET,
                                                             ESP_NETIF_DOMAIN_NAME_SERVER,
                                                             &offer, sizeof(offer)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns));
    }

    esp_netif_action_start(s_usb_netif, 0, 0, 0);

    if (lan) {
        esp_netif_action_connected(s_usb_netif, 0, 0, 0);
        tud_network_link_state(0, true);
        ESP_LOGI(TAG, "USB LAN %s", USB_LAN_IP_STR);
    } else {
        ESP_RETURN_ON_ERROR(usb_start_stack(usb_mac), TAG, "USB net init");
        if (tud_mounted()) {
            usb_set_connected(true);
        }
    }

    ESP_LOGI(TAG, "USB %s %s host MAC " MACSTR " lwIP MAC " MACSTR,
             app_boot_firmware_is_ncm() ? "NCM" : "RNDIS",
             lan ? "LAN" : "WAN",
             MAC2STR(usb_mac), MAC2STR(lwip_mac));
    return ESP_OK;
}

esp_err_t usb_wan_start(void)
{
    return usb_net_start(false);
}

esp_err_t usb_lan_start(void)
{
    return usb_net_start(true);
}
