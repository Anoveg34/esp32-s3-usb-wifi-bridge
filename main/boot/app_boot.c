#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "app_boot.h"
#include "app_config.h"
#include "sdkconfig.h"

static const char *TAG = "boot";

bool app_boot_firmware_is_ncm(void)
{
#if CONFIG_EXAMPLE_USB_NET_MODE_NCM
    return true;
#else
    return false;
#endif
}

static const esp_partition_t *usb_slot(bool ncm)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ncm ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                        : ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                    NULL);
}

bool app_boot_slot_ready(bool ncm)
{
    const esp_partition_t *part = usb_slot(ncm);
    if (part == NULL) {
        return false;
    }
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

esp_err_t app_boot_select_usb(bool ncm)
{
    const esp_partition_t *part = usb_slot(ncm);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        ESP_LOGW(TAG, "%s slot has no valid firmware (flash both images)",
                 ncm ? "NCM/ota_1" : "RNDIS/ota_0");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->address == part->address) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "next boot %s (%s v%s)", ncm ? "NCM" : "RNDIS",
             desc.project_name, desc.version);
    return esp_ota_set_boot_partition(part);
}

void app_boot_sync_from_nvs(void)
{
    const bool want_ncm = app_config_get()->usb_ncm;
    if (want_ncm == app_boot_firmware_is_ncm()) {
        return;
    }
    const esp_partition_t *part = usb_slot(want_ncm);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (part == NULL || !app_boot_slot_ready(want_ncm)) {
        ESP_LOGW(TAG, "NVS wants %s but that slot is empty; stay on current firmware",
                 want_ncm ? "NCM" : "RNDIS");
        return;
    }
    if (running && running->address == part->address) {
        return;
    }
    ESP_LOGI(TAG, "NVS USB mode differs, reboot into %s", want_ncm ? "NCM" : "RNDIS");
    if (app_boot_select_usb(want_ncm) == ESP_OK) {
        esp_restart();
    }
}
