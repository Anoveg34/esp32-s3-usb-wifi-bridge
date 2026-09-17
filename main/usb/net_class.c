#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "tinyusb_net.h"
#include "tusb.h"
#include "class/net/net_device.h"
#include "device/usbd_pvt.h"

typedef struct {
    void *buffer;
    void *buff_free_arg;
    uint16_t len;
    esp_err_t result;
} packet_t;

struct tinyusb_net_handle {
    bool initialized;
    SemaphoreHandle_t buffer_sema;
    EventGroupHandle_t tx_flags;
    tusb_net_rx_cb_t rx_cb;
    tusb_net_free_tx_cb_t tx_buff_free_cb;
    tusb_net_init_cb_t init_cb;
    void *ctx;
    packet_t *packet_to_send;
};

static const int TX_FINISHED_BIT = BIT0;
static struct tinyusb_net_handle s_net_obj;
static const char *TAG = "tusb_net";

static void do_send_sync(void *ctx)
{
    (void)ctx;
    if (xSemaphoreTake(s_net_obj.buffer_sema, 0) != pdTRUE || s_net_obj.packet_to_send == NULL) {
        return;
    }

    packet_t *packet = s_net_obj.packet_to_send;
    if (tud_network_can_xmit(packet->len)) {
        tud_network_xmit(packet, packet->len);
        packet->result = ESP_OK;
    } else {
        packet->result = ESP_FAIL;
    }
    xSemaphoreGive(s_net_obj.buffer_sema);
    xEventGroupSetBits(s_net_obj.tx_flags, TX_FINISHED_BIT);
}

esp_err_t tinyusb_net_send_sync(void *buffer, uint16_t len, void *buff_free_arg, TickType_t timeout)
{
    if (!tud_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_net_obj.tx_flags) {
        s_net_obj.tx_flags = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_net_obj.tx_flags, ESP_ERR_NO_MEM, TAG, "Failed to allocate event flags");
    }
    if (!s_net_obj.buffer_sema) {
        s_net_obj.buffer_sema = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_net_obj.buffer_sema, ESP_ERR_NO_MEM, TAG, "Failed to allocate buffer semaphore");
    }

    packet_t packet = {
        .buffer = buffer,
        .len = len,
        .buff_free_arg = buff_free_arg,
        .result = ESP_FAIL,
    };
    s_net_obj.packet_to_send = &packet;
    xSemaphoreGive(s_net_obj.buffer_sema);

    usbd_defer_func(do_send_sync, NULL, false);

    EventBits_t bits = xEventGroupWaitBits(s_net_obj.tx_flags, TX_FINISHED_BIT, pdTRUE, pdTRUE, timeout);
    xSemaphoreTake(s_net_obj.buffer_sema, portMAX_DELAY);
    s_net_obj.packet_to_send = NULL;
    if (bits & TX_FINISHED_BIT) {
        return packet.result;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t tinyusb_net_init(const tinyusb_net_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_net_obj.initialized == false, ESP_ERR_INVALID_STATE, TAG, "TinyUSB Net class is already initialized");
    s_net_obj.rx_cb = cfg->on_recv_callback;
    s_net_obj.init_cb = cfg->on_init_callback;
    s_net_obj.tx_buff_free_cb = cfg->free_tx_buffer;
    s_net_obj.ctx = cfg->user_context;
    s_net_obj.initialized = true;
    return ESP_OK;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (s_net_obj.rx_cb) {
        s_net_obj.rx_cb((void *)src, size, s_net_obj.ctx);
    }
    tud_network_recv_renew();
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    packet_t *packet = ref;
    uint16_t len = arg;
    memcpy(dst, packet->buffer, packet->len);
    if (s_net_obj.tx_buff_free_cb) {
        s_net_obj.tx_buff_free_cb(packet->buff_free_arg, s_net_obj.ctx);
    }
    return len;
}

void tud_network_init_cb(void)
{
    if (s_net_obj.init_cb) {
        s_net_obj.init_cb(s_net_obj.ctx);
    }
}
