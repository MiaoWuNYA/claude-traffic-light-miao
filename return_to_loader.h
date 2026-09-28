/*
 * return_to_loader.h — drop-in for ROM firmware
 *
 * Include this header and call return_to_loader_setup() as the very
 * first line of app_main().  It points the OTA boot partition back to
 * the factory (loader) partition, so that any reset, power-cycle, or
 * esp_restart() returns to the ROM Loader instead of re-entering this
 * ROM.
 *
 * Usage:
 *   #include "return_to_loader.h"
 *
 *   void app_main(void) {
 *       return_to_loader_setup();   // ← must be first
 *       // ... normal ROM init ...
 *   }
 *
 * If the running firmware is NOT on an OTA partition (e.g. it was
 * flashed directly to factory via esptool), this function is a no-op.
 */

#pragma once

#include "esp_ota_ops.h"

static inline void return_to_loader_setup(void)
{
    const esp_partition_t *cur = esp_ota_get_running_partition();
    if (!cur || cur->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0)
        return;

    const esp_partition_t *fac = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (fac)
        esp_ota_set_boot_partition(fac);
}
