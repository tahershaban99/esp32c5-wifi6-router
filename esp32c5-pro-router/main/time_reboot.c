/**
 * @file time_reboot.c
 * @brief SNTP time sync, daily scheduled auto-reboot, countdown timer & boot reporting.
 */

#include "router_config.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_reboot";

static bool s_boot_report_sent = false;
static int s_last_reboot_day = -1;

static void apply_timezone(int8_t tz_offset) {
    char tz_buf[32];
    // POSIX TZ format: sign is inverted (UTC+2 is GMT-2)
    int inv = -tz_offset;
    snprintf(tz_buf, sizeof(tz_buf), "UTC%s%d", (inv >= 0 ? "+" : ""), inv);
    setenv("TZ", tz_buf, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s (Offset UTC%+d)", tz_buf, tz_offset);
}

static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP synchronization successful!");
    g_router_status.ntp_synced = true;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(g_router_status.current_time_str, sizeof(g_router_status.current_time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Send Telegram boot report if not sent yet
    if (!s_boot_report_sent && g_router_cfg.tg_enabled && strlen(g_router_cfg.tg_admin_chat_id) > 0) {
        s_boot_report_sent = true;
        char report[512];
        char cur_part[16] = "ota_0", next_part[16] = "ota_1";
        bool rb = true;
        ota_manager_get_info(cur_part, next_part, &rb);

        snprintf(report, sizeof(report),
                 "🚀 <b>تقرير إقلاع راوتر ESP32-C5 Mini بنجاح:</b>\n\n"
                 "📶 <b>الراوتر الأساسي:</b> %s (%s)\n"
                 "🧠 <b>الذاكرة المتاحة (RAM):</b> %lu KB\n"
                 "⚡ <b>تردد المعالج:</b> %u MHz RISC-V\n"
                 "💾 <b>قسم الإقلاع:</b> %s\n"
                 "🕒 <b>توقيت الراوتر:</b> %s\n"
                 "🛡️ <b>الحماية:</b> Anti-Rollback مفعل ✅",
                 g_router_status.sta_active_ssid, g_router_status.sta_band_str,
                 (unsigned long)(esp_get_free_heap_size() / 1024),
                 g_router_cfg.cpu_freq_mhz, cur_part,
                 g_router_status.current_time_str);

        telegram_bot_send_alert(g_router_cfg.tg_admin_chat_id, report);
    }
}

static void time_reboot_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        g_router_status.uptime_seconds++;
        g_router_status.free_heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        g_router_status.min_heap_kb  = (uint32_t)(esp_get_minimum_free_heap_size() / 1024);

        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        if (g_router_status.ntp_synced) {
            strftime(g_router_status.current_time_str, sizeof(g_router_status.current_time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
        } else {
            snprintf(g_router_status.current_time_str, sizeof(g_router_status.current_time_str), "غير متزامن NTP");
        }

        // 1. Check Countdown Reboot
        if (g_router_status.countdown_reboot_target_epoch > 0) {
            if (now >= g_router_status.countdown_reboot_target_epoch) {
                ESP_LOGW(TAG, "Countdown reboot timer elapsed. Rebooting now...");
                storage_nvs_save_counters();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }

        // 2. Check Daily Scheduled Auto-Reboot
        if (g_router_cfg.reboot_enabled && g_router_status.ntp_synced) {
            int target_hour = 3, target_min = 0;
            if (sscanf(g_router_cfg.reboot_time, "%d:%d", &target_hour, &target_min) == 2) {
                if (timeinfo.tm_hour == target_hour && timeinfo.tm_min == target_min &&
                    s_last_reboot_day != timeinfo.tm_yday) {
                    s_last_reboot_day = timeinfo.tm_yday;
                    ESP_LOGI(TAG, "Daily scheduled auto-reboot triggered (%02d:%02d)", target_hour, target_min);

                    if (g_router_cfg.tg_enabled && strlen(g_router_cfg.tg_admin_chat_id) > 0) {
                        telegram_bot_send_alert(g_router_cfg.tg_admin_chat_id,
                            "🔄 <b>إعادة تشغيل يومية مجدولة:</b>\nجاري حفظ العدادات وإعادة تشغيل الراوتر لإنعاش الذاكرة...");
                        vTaskDelay(pdMS_TO_TICKS(1500));
                    }
                    storage_nvs_save_counters();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        }
    }
}

void time_reboot_set_countdown(int minutes) {
    if (minutes <= 0) {
        time_reboot_trigger_immediate();
        return;
    }
    time_t now;
    time(&now);
    g_router_status.countdown_reboot_target_epoch = now + (minutes * 60);
    ESP_LOGI(TAG, "Set countdown reboot for %d minutes from now (epoch %lld)", minutes, (long long)g_router_status.countdown_reboot_target_epoch);
}

void time_reboot_trigger_immediate(void) {
    ESP_LOGW(TAG, "Immediate reboot requested! Flushing NVS...");
    storage_nvs_save_counters();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t time_reboot_init(void) {
    apply_timezone(g_router_cfg.tz_offset_hours);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.windows.com");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    xTaskCreate(time_reboot_task, "time_reboot", 3072, NULL, 1, NULL);
    return ESP_OK;
}

