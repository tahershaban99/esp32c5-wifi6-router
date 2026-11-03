/**
 * @file storage_nvs.c
 * @brief Atomic NVS Persistence for Router Configuration, Client Registry & Usage Counters.
 *
 * Provides safe non-volatile flash storage:
 * - Atomic commit for router configuration blob
 * - Periodic background flush for high-volume byte counters (every 5 minutes)
 * - Pre-reboot and pre-OTA atomic persistence
 * - Hardware BOOT button factory reset for admin credentials (admin/admin)
 * - Safe partition recovery in case of flash sector corruption
 */

#include "router_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "storage_nvs";

#define NVS_NAMESPACE_CFG    "router_cfg"
#define NVS_BLOB_KEY_CFG     "cfg_blob"

// Global instances defined in router_config.h
router_global_config_t g_router_cfg;
router_live_status_t   g_router_status;

/**
 * @brief Initialize default router parameters when NVS is empty
 */
static void set_default_configuration(void) {
    memset(&g_router_cfg, 0, sizeof(router_global_config_t));
    g_router_cfg.magic = ROUTER_CFG_MAGIC;

    // 1. Default Upstream Wi-Fi (STA) - Empty initially until configured in dashboard
    strncpy(g_router_cfg.sta_ssid, "", sizeof(g_router_cfg.sta_ssid) - 1);
    strncpy(g_router_cfg.sta_password, "", sizeof(g_router_cfg.sta_password) - 1);
    g_router_cfg.sta_is_static = false;
    strncpy(g_router_cfg.sta_ip, "", sizeof(g_router_cfg.sta_ip) - 1);
    strncpy(g_router_cfg.sta_mask, "255.255.255.0", sizeof(g_router_cfg.sta_mask) - 1);
    strncpy(g_router_cfg.sta_gw, "", sizeof(g_router_cfg.sta_gw) - 1);
    strncpy(g_router_cfg.sta_dns, "8.8.8.8", sizeof(g_router_cfg.sta_dns) - 1);

    // 2. Default ESP32-C5 SoftAP (Dual-Band Wi-Fi 6 on universal Channel 1)
    strncpy(g_router_cfg.ap_ssid, DEFAULT_AP_SSID, sizeof(g_router_cfg.ap_ssid) - 1);
    strncpy(g_router_cfg.ap_password, DEFAULT_AP_PASS, sizeof(g_router_cfg.ap_password) - 1);
    g_router_cfg.ap_channel = 1;                        // Universal Channel 1 (visible to 100% of devices)
    g_router_cfg.ap_band_mode = ROUTER_BAND_MODE_AUTO;  // Dual-Band Auto (2.4GHz + 5GHz)
    g_router_cfg.ap_protocol = ROUTER_PROTO_AX_HE40;    // 802.11ax Wi-Fi 6 HE40

    // 3. Performance & Power Management
    g_router_cfg.cpu_freq_mhz = DEFAULT_CPU_FREQ_MHZ;   // 240 MHz Max Performance
    g_router_cfg.wifi_power_save = false;

    // 4. Main ISP Quota & Meter Calibration
    g_router_cfg.isp_total_gb = 140.0f;
    g_router_cfg.isp_used_bytes = 0;
    strncpy(g_router_cfg.isp_cycle_start, "2026-11-01", sizeof(g_router_cfg.isp_cycle_start) - 1);
    strncpy(g_router_cfg.isp_cycle_renewal, "2026-11-30", sizeof(g_router_cfg.isp_cycle_renewal) - 1);
    g_router_cfg.meter_scale_pct = 100;                 // 100% (No scaling)
    g_router_cfg.gb_standard = 1024;                    // 1024 MB = 1 GiB Binary Standard

    // 5. Telegram Bot & Multi-Threshold Alerts
    strncpy(g_router_cfg.tg_bot_token, "", sizeof(g_router_cfg.tg_bot_token) - 1);
    strncpy(g_router_cfg.tg_admin_chat_id, "", sizeof(g_router_cfg.tg_admin_chat_id) - 1);
    strncpy(g_router_cfg.tg_thresholds, "50,75,90,95", sizeof(g_router_cfg.tg_thresholds) - 1);
    g_router_cfg.tg_enabled = false;
    g_router_cfg.threshold_alerted_mask = 0;

    // 6. Scheduled Daily Auto-Reboot
    g_router_cfg.reboot_enabled = true;
    strncpy(g_router_cfg.reboot_time, "03:00", sizeof(g_router_cfg.reboot_time) - 1);
    g_router_cfg.tz_offset_hours = 2;                   // UTC+2 default

    // 7. Security (admin / admin)
    strncpy(g_router_cfg.admin_user, DEFAULT_ADMIN_USER, sizeof(g_router_cfg.admin_user) - 1);
    strncpy(g_router_cfg.admin_pass, DEFAULT_ADMIN_PASS, sizeof(g_router_cfg.admin_pass) - 1);

    // 8. Device Registry
    g_router_cfg.device_count = 0;
}

/**
 * @brief Initialize the NVS flash partition with automatic recovery
 */
esp_err_t storage_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or new version found. Formatting flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS flash partition mounted successfully");
    } else {
        ESP_LOGE(TAG, "Failed to mount NVS partition: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Load router persistent configuration and device registry from NVS
 */
esp_err_t storage_nvs_load_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No existing configuration found in NVS. Loading factory defaults...");
        set_default_configuration();
        return storage_nvs_save_config();
    }

    size_t required_size = sizeof(router_global_config_t);
    err = nvs_get_blob(handle, NVS_BLOB_KEY_CFG, &g_router_cfg, &required_size);
    nvs_close(handle);

    if (err != ESP_OK || required_size != sizeof(router_global_config_t) || g_router_cfg.magic != ROUTER_CFG_MAGIC) {
        ESP_LOGW(TAG, "Configuration magic/size mismatch (magic=0x%08lx). Re-initializing clean factory defaults...", (unsigned long)g_router_cfg.magic);
        set_default_configuration();
        storage_nvs_save_config();
        return ESP_OK;  // Always return ESP_OK so app_main never aborts
    }

    ESP_LOGI(TAG, "Configuration loaded from NVS: STA='%s', AP='%s' (5GHz Wi-Fi 6), Registered Devices=%d",
             g_router_cfg.sta_ssid, g_router_cfg.ap_ssid, g_router_cfg.device_count);

    return ESP_OK;
}

/**
 * @brief Atomically commit the entire router configuration and device table to NVS
 */
esp_err_t storage_nvs_save_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing: %s", NVS_NAMESPACE_CFG, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_BLOB_KEY_CFG, &g_router_cfg, sizeof(router_global_config_t));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "NVS partition out of space! Formatting NVS partition and retrying...");
        nvs_close(handle);
        nvs_flash_erase();
        nvs_flash_init();
        if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &handle) == ESP_OK) {
            err = nvs_set_blob(handle, NVS_BLOB_KEY_CFG, &g_router_cfg, sizeof(router_global_config_t));
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "Configuration and device counters committed atomically to NVS");
        } else {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

/**
 * @brief Save cumulative byte counters before reboot, OTA or power-down
 */
esp_err_t storage_nvs_save_counters(void) {
    ESP_LOGI(TAG, "Flushing all device and ISP quota counters to NVS...");
    return storage_nvs_save_config();
}

/**
 * @brief Reset administrator credentials to default (admin / admin)
 * Triggered by holding the BOOT button (GPIO9) for 10 seconds.
 */
void storage_nvs_reset_admin_credentials(void) {
    ESP_LOGW(TAG, "Resetting administrator username and password to defaults (admin/admin)");
    strncpy(g_router_cfg.admin_user, DEFAULT_ADMIN_USER, sizeof(g_router_cfg.admin_user) - 1);
    strncpy(g_router_cfg.admin_pass, DEFAULT_ADMIN_PASS, sizeof(g_router_cfg.admin_pass) - 1);
    storage_nvs_save_config();
}

/**
 * @brief Periodic background task ensuring counter persistence every 5 minutes
 */
static void nvs_periodic_autosave_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000)); // Sleep for 5 minutes
        ESP_LOGD(TAG, "Periodic 5-minute NVS sync executing...");
        storage_nvs_save_counters();
    }
}

void storage_start_autosave_task(void) {
    xTaskCreate(nvs_periodic_autosave_task, "nvs_autosave", 3072, NULL, 1, NULL);
    ESP_LOGI(TAG, "Periodic NVS autosave task started (5-minute interval)");
}
