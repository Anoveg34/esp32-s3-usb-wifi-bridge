#pragma once

#include <stdbool.h>
#include "esp_err.h"

bool app_boot_firmware_is_ncm(void);
bool app_boot_slot_ready(bool ncm);
esp_err_t app_boot_select_usb(bool ncm);
void app_boot_sync_from_nvs(void);
