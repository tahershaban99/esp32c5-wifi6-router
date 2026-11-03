/**
 * @file ota_manager.c
 * @brief Dual-Bank Local & Cloud OTA firmware updates with Anti-Rollback support.
 */

#include "router_config.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_manager";

esp_err_t ota_manager_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running firmware partition: %s (Offset 0x%lx, Size 0x%lx)",
             running->label, running->address, running->size);

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "First boot after OTA upgrade! Validating app and cancelling rollback...");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Firmware successfully validated and committed permanently!");
            } else {
                ESP_LOGE(TAG, "Failed to cancel rollback: %s", esp_err_to_name(err));
            }
        }
    }
    return ESP_OK;
}

void ota_manager_get_info(char *cur_part, char *next_part, bool *rollback_valid) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    if (cur_part) strncpy(cur_part, running ? running->label : "ota_0", 15);
    if (next_part) strncpy(next_part, next ? next->label : "ota_1", 15);
    if (rollback_valid) *rollback_valid = true;
}

static void url_ota_task(void *pvParameters) {
    char *url = (char *)pvParameters;
    ESP_LOGI(TAG, "Starting Remote HTTPS OTA from URL: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Download & Flash succeeded! Flushing NVS and restarting...");
        storage_nvs_save_counters();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Remote OTA upgrade failed: %s", esp_err_to_name(ret));
    }

    free(url);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_url(const char *url) {
    if (!url || strlen(url) == 0) return ESP_ERR_INVALID_ARG;
    char *url_copy = strdup(url);
    if (!url_copy) return ESP_ERR_NO_MEM;

    xTaskCreate(url_ota_task, "url_ota", 8192, url_copy, 2, NULL);
    return ESP_OK;
}

