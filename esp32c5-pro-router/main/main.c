/**
 * @file main.c
 * @brief Application Entry Point, RISC-V PM, GPIO9 Reset Task & Subsystem Orchestration.
 */

#include "router_config.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

extern void storage_start_autosave_task(void);

/**
 * @brief Hardware Factory Reset Task monitoring GPIO9 (BOOT Button on ESP32-C5)
 * If held continuously for 10 seconds, resets admin credentials to admin/admin.
 */
static void factory_reset_button_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_FACTORY_RESET_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int hold_time_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (gpio_get_level(GPIO_FACTORY_RESET_PIN) == 0) {
            // Button is pressed (Active LOW)
            hold_time_ms += 100;
            if (hold_time_ms % 1000 == 0) {
                ESP_LOGI(TAG, "BOOT button held for %d seconds (Keep holding for 10s to reset admin credentials)...", hold_time_ms / 1000);
            }
            if (hold_time_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "*****************************************************");
                ESP_LOGW(TAG, "  HARDWARE FACTORY RESET TRIGGERED! (GPIO9 10s HOLD) ");
                ESP_LOGW(TAG, "  Resetting admin credentials to default: admin/admin ");
                ESP_LOGW(TAG, "*****************************************************");
                storage_nvs_reset_admin_credentials();

                // Wait until button released
                while (gpio_get_level(GPIO_FACTORY_RESET_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                hold_time_ms = 0;
            }
        } else {
            hold_time_ms = 0;
        }
    }
}

/**
 * @brief Configure RISC-V Power Management for ESP32-C5
 */
static void init_power_management(void) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = g_router_cfg.cpu_freq_mhz ? g_router_cfg.cpu_freq_mhz : 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ESP32-C5 Power Management configured: Max Freq = %d MHz, Min Freq = 80 MHz", pm_config.max_freq_mhz);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure returned: %s", esp_err_to_name(err));
    }
#endif
}

void app_main(void) {
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "   ESP32-C5 Mini Dual-Band (2.4GHz & 5GHz) Wi-Fi 6 Router ");
    ESP_LOGI(TAG, "   Firmware: %s | Hardware: %s ", ROUTER_FIRMWARE_VERSION, ROUTER_HARDWARE_TARGET);
    ESP_LOGI(TAG, "=========================================================");

    // 1. Initialize NVS Flash & Load Persistent Configuration
    esp_err_t nvs_err = storage_nvs_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s), formatting flash...", esp_err_to_name(nvs_err));
        nvs_flash_erase();
        storage_nvs_init();
    }
    nvs_err = storage_nvs_load_config();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load config warning (%s), operating with factory defaults", esp_err_to_name(nvs_err));
    }

    // 2. Configure RISC-V Power Management
    init_power_management();

    // 3. Start Hardware Factory Reset Button Task on GPIO9 (Core 0, Priority 2)
    xTaskCreate(factory_reset_button_task, "reset_btn_task", 2048, NULL, 2, NULL);

    // 4. Initialize ESP32-C5 Wi-Fi 6 & 5GHz NAPT Subsystem
    ESP_ERROR_CHECK(wifi_5g_napt_init());

    // 5. Initialize High-Performance Packet Shaper, Token Bucket & IRAM hooks
    ESP_ERROR_CHECK(packet_shaper_init());

    // 6. Initialize SNTP Time Synchronization & Daily Auto-Reboot Engine
    ESP_ERROR_CHECK(time_reboot_init());

    // 7. Initialize Telegram Bot HTTPS Client & Alerts
    ESP_ERROR_CHECK(telegram_bot_init());

    // 8. Start Web Server with Embedded Gzipped Dashboard & REST APIs
    esp_err_t ws_err = web_server_start();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Web Server: %s", esp_err_to_name(ws_err));
    }

    // 9. Initialize OTA Manager and Validate Running Partition (Cancel Rollback)
    esp_err_t ota_err = ota_manager_init();
    if (ota_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA Manager init warning: %s", esp_err_to_name(ota_err));
    }

    // 10. Start Periodic NVS Autosave Task
    storage_start_autosave_task();

    ESP_LOGI(TAG, "All ESP32-C5 router subsystems started successfully!");
}

