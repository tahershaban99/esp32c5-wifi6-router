/**
 * @file telegram_bot.c
 * @brief Telegram Bot HTTPS Client, Multi-Threshold Quota Alerts & Long-Polling Engine.
 *
 * Implements:
 * - HTTPS client using esp_http_client with mbedTLS certificate bundle
 * - Non-blocking Long-Polling task on Core 0 for real-time remote commands
 * - Multi-threshold proactive push alerts for main ISP quota (50%, 75%, 90%, 95%)
 * - Dedicated per-device quota exhaustion alerts (sent to admin + device's tgChat)
 * - Remote router administration (/quota, /devices, /speed, /block, /unblock, /status, /ota, /reboot)
 */

#include "router_config.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "telegram_bot";

static int64_t s_last_update_id = 0;

/**
 * @brief Universal command execution engine used by both Telegram bot and Web UI console
 */
void telegram_execute_command(const char *cmd, char *out_buf, size_t out_max) {
    if (!cmd || !out_buf || out_max == 0) return;

    // Convert command to lowercase
    char c[128];
    strncpy(c, cmd, sizeof(c) - 1);
    c[sizeof(c) - 1] = '\0';
    for (int i = 0; c[i]; i++) {
        if (c[i] >= 'A' && c[i] <= 'Z') c[i] += 32;
    }

    if (strstr(c, "/quota") || strstr(c, "استهلاك") || strstr(c, "باقي")) {
        double std_div = (g_router_cfg.gb_standard == 1000) ? 1000000000.0 : 1073741824.0;
        double used_gb = (double)g_router_cfg.isp_used_bytes / std_div;
        double total_gb = (double)g_router_cfg.isp_total_gb;
        double rem_gb = (total_gb > used_gb) ? (total_gb - used_gb) : 0.0;
        double pct = (total_gb > 0) ? (used_gb / total_gb * 100.0) : 0.0;

        int pos = snprintf(out_buf, out_max,
            "📊 <b>تقرير استهلاك باقة الإنترنت (ESP32-C5 Router):</b>\n\n"
            "📥 <b>المستهلك:</b> %.2f GB (%.1f%%)\n"
            "🎯 <b>سعة الباقة:</b> %.2f GB\n"
            "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
            "📅 <b>تاريخ التجديد:</b> %s\n"
            "⚡ <b>الأجهزة النشطة:</b> %d / 12 متصل\n"
            "🚀 <b>السرعة الحالية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n\n"
            "📱 <b>باقات الأجهزة المحددة:</b>\n",
            used_gb, pct, total_gb, rem_gb, g_router_cfg.isp_cycle_renewal,
            g_router_status.ap_clients_count,
            g_router_status.cur_down_mbps, g_router_status.cur_up_mbps);

        int listed = 0;
        for (int i = 0; i < g_router_cfg.device_count; i++) {
            router_device_t *d = &g_router_cfg.devices[i];
            if (d->quota_gb > 0.0f) {
                listed++;
                double d_used = (double)(d->rx_bytes + d->tx_bytes) / std_div;
                double d_pct = (d_used / (double)d->quota_gb) * 100.0;
                pos += snprintf(out_buf + pos, (pos < out_max ? out_max - pos : 0),
                    "• <b>%s</b>: %.2f / %.1f GB (%.0f%%) %s\n",
                    d->name, d_used, d->quota_gb, d_pct,
                    d->quota_exceeded ? "🚫" : (d_pct >= 80 ? "⚠️" : "🟢"));
            }
        }
        if (listed == 0) {
            snprintf(out_buf + pos, (pos < out_max ? out_max - pos : 0), "<i>لا توجد باقات كوتة مخصصة للأجهزة حالياً.</i>");
        }

    } else if (strstr(c, "/devices") || strstr(c, "أجهزة") || strstr(c, "المتصلين")) {
        int pos = snprintf(out_buf, out_max, "👥 <b>الأجهزة المتصلة حالياً (SoftAP & Bridge):</b>\n\n");
        int count = 0;
        for (int i = 0; i < g_router_cfg.device_count; i++) {
            router_device_t *d = &g_router_cfg.devices[i];
            if (d->is_connected) {
                count++;
                pos += snprintf(out_buf + pos, (pos < out_max ? out_max - pos : 0),
                    "• <b>%s</b> (<code>%s</code>)\n"
                    "  ⬇️ %.2f Mbps | ⬆️ %.2f Mbps %s%s\n",
                    d->name, d->ip, d->down_mbps, d->up_mbps,
                    d->is_gaming ? "🎮 VIP" : "",
                    d->is_blocked ? "🚫 محظور" : (d->quota_exceeded ? "⚠️ كوتة ممتلئة" : "🟢"));
            }
        }
        if (count == 0) {
            snprintf(out_buf + pos, (pos < out_max ? out_max - pos : 0), "لا توجد أجهزة متصلة حالياً.");
        }

    } else if (strstr(c, "/speed") || strstr(c, "سرعة")) {
        snprintf(out_buf, out_max,
            "🚀 <b>اختبار السرعة ونطاق التردد (ESP32-C5 5GHz):</b>\n\n"
            "📥 <b>سرعة التحميل:</b> %.2f Mbps\n"
            "📤 <b>سرعة الرفع:</b> %.2f Mbps\n"
            "👥 <b>الأجهزة النشطة:</b> %d متصل\n"
            "📶 <b>نطاق التردد:</b> %s (Ch %d)\n"
            "⚡ <b>قوة الإشارة:</b> %d dBm\n"
            "🌐 <b>عرض النطاق:</b> 40 MHz (Wi-Fi 6 HE40)",
            g_router_status.cur_down_mbps, g_router_status.cur_up_mbps,
            g_router_status.ap_clients_count,
            g_router_status.sta_band_str, g_router_status.sta_channel,
            g_router_status.sta_rssi);

    } else if (strstr(c, "/block") && !strstr(c, "/unblock")) {
        char target_ip[32] = "";
        sscanf(cmd, "%*s %31s", target_ip);
        router_device_t *found = NULL;
        for (int i = 0; i < g_router_cfg.device_count; i++) {
            if (strcmp(g_router_cfg.devices[i].ip, target_ip) == 0) {
                found = &g_router_cfg.devices[i];
                break;
            }
        }
        if (found) {
            found->is_blocked = true;
            storage_nvs_save_config();
            snprintf(out_buf, out_max,
                "🚫 <b>تم حظر الجهاز بنجاح!</b>\n\n"
                "📱 <b>الجهاز:</b> %s\n"
                "🌐 <b>IP:</b> %s\n"
                "⚡ تم قطع وصول الإنترنت فوراً عبر lwIP NAPT hook.",
                found->name, found->ip);
        } else {
            snprintf(out_buf, out_max, "⚠️ لم يتم العثور على جهاز بالـ IP: %s\nمثال للاستخدام: <code>/block 192.168.4.2</code>", target_ip);
        }

    } else if (strstr(c, "/unblock")) {
        char target_ip[32] = "";
        sscanf(cmd, "%*s %31s", target_ip);
        router_device_t *found = NULL;
        for (int i = 0; i < g_router_cfg.device_count; i++) {
            if (strcmp(g_router_cfg.devices[i].ip, target_ip) == 0) {
                found = &g_router_cfg.devices[i];
                break;
            }
        }
        if (found) {
            found->is_blocked = false;
            found->quota_exceeded = false;
            storage_nvs_save_config();
            snprintf(out_buf, out_max,
                "✅ <b>تم إلغاء حظر الجهاز بنجاح!</b>\n\n"
                "📱 <b>الجهاز:</b> %s\n"
                "🌐 <b>IP:</b> %s\n"
                "⚡ تمت استعادة اتصال الإنترنت للجهاز بالكامل.",
                found->name, found->ip);
        } else {
            snprintf(out_buf, out_max, "⚠️ لم يتم العثور على جهاز بالـ IP: %s\nمثال للاستخدام: <code>/unblock 192.168.4.2</code>", target_ip);
        }

    } else if (strstr(c, "/status") || strstr(c, "حالة")) {
        int64_t s = g_router_status.uptime_seconds;
        int d = (int)(s / 86400);
        int h = (int)((s % 86400) / 3600);
        int m = (int)((s % 3600) / 60);

        snprintf(out_buf, out_max,
            "⚙️ <b>حالة نظام راوتر ESP32-C5 Mini:</b>\n\n"
            "⏱️ <b>مدة التشغيل:</b> %dي %dس %dد\n"
            "🧠 <b>الذاكرة المتاحة (RAM):</b> %lu KB\n"
            "⚡ <b>تردد المعالج:</b> %u MHz RISC-V Single-Core\n"
            "📶 <b>الراوتر الأساسي:</b> %s (%s, %d dBm)\n"
            "👥 <b>الأجهزة المتصلة:</b> %d / 12 متصل\n"
            "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
            "🕒 <b>توقيت الراوتر:</b> %s",
            d, h, m, (unsigned long)(esp_get_free_heap_size() / 1024),
            g_router_cfg.cpu_freq_mhz, g_router_status.sta_active_ssid,
            g_router_status.sta_band_str, g_router_status.sta_rssi,
            g_router_status.ap_clients_count,
            g_router_status.cur_down_mbps, g_router_status.cur_up_mbps,
            g_router_status.current_time_str);

    } else if (strstr(c, "/ota") || strstr(c, "تحديث")) {
        char cur_part[16] = "ota_0", next_part[16] = "ota_1";
        bool rb = true;
        ota_manager_get_info(cur_part, next_part, &rb);

        snprintf(out_buf, out_max,
            "🔄 <b>نظام التحديث الهوائي (Dual-Bank OTA):</b>\n\n"
            "🏷️ <b>الإصدار الحالي:</b> %s\n"
            "🕒 <b>تاريخ البناء:</b> %s - %s\n"
            "💾 <b>القسم النشط:</b> %s\n"
            "🎯 <b>القسم البديل:</b> %s\n"
            "🛡️ <b>الحماية من الأعطال:</b> Anti-Rollback مفعل ✅",
            ROUTER_FIRMWARE_VERSION, __DATE__, __TIME__, cur_part, next_part);

    } else if (strstr(c, "/reboot") || strstr(c, "ريستارت")) {
        snprintf(out_buf, out_max, "🔄 <b>جاري حفظ العدادات وإعادة تشغيل الراوتر الآن بأمان...</b>");
        time_reboot_set_countdown(1);

    } else {
        snprintf(out_buf, out_max,
            "👋 <b>مرحباً بك في بوت راوتر ESP32-C5 Mini</b> 🌐\n\n"
            "الأوامر المتاحة:\n"
            "🔹 <code>/quota</code> : رصيد الباقة واستهلاك الأجهزة\n"
            "🔹 <code>/devices</code> : تفاصيل وسرعات الأجهزة المتصلة\n"
            "🔹 <code>/speed</code> : اختبار السرعة ونطاق 5GHz\n"
            "🔹 <code>/block [IP]</code> : حظر جهاز من الإنترنت\n"
            "🔹 <code>/unblock [IP]</code> : فك حظر جهاز\n"
            "🔹 <code>/status</code> : تقرير حالة النظام والمعالج\n"
            "🔹 <code>/ota</code> : إصدار النظام وقسم الإقلاع\n"
            "🔹 <code>/reboot</code> : إعادة تشغيل الراوتر بأمان");
    }
}

/**
 * @brief Send an alert or reply via Telegram HTTPS API
 */
esp_err_t telegram_bot_send_alert(const char *chat_id, const char *markdown_msg) {
    if (!g_router_cfg.tg_enabled || strlen(g_router_cfg.tg_bot_token) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!chat_id || strlen(chat_id) == 0) {
        chat_id = g_router_cfg.tg_admin_chat_id;
    }
    if (strlen(chat_id) == 0) return ESP_ERR_INVALID_ARG;

    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", g_router_cfg.tg_bot_token);

    // Escape message for JSON payload
    char *escaped_msg = malloc(strlen(markdown_msg) * 2 + 1);
    if (!escaped_msg) return ESP_ERR_NO_MEM;
    int j = 0;
    for (int i = 0; markdown_msg[i]; i++) {
        if (markdown_msg[i] == '"') {
            escaped_msg[j++] = '\\';
            escaped_msg[j++] = '"';
        } else if (markdown_msg[i] == '\n') {
            escaped_msg[j++] = '\\';
            escaped_msg[j++] = 'n';
        } else if (markdown_msg[i] == '\r') {
            // skip
        } else {
            escaped_msg[j++] = markdown_msg[i];
        }
    }
    escaped_msg[j] = '\0';

    char *post_data = malloc(strlen(escaped_msg) + 128);
    if (!post_data) {
        free(escaped_msg);
        return ESP_ERR_NO_MEM;
    }
    snprintf(post_data, strlen(escaped_msg) + 128,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
             chat_id, escaped_msg);
    free(escaped_msg);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram alert delivered to chat %s (HTTP %d)",
                 chat_id, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Failed to send Telegram alert: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(post_data);
    return err;
}

void telegram_bot_test_alert(void) {
    char test_msg[256];
    snprintf(test_msg, sizeof(test_msg),
             "🧪 <b>اختبار إشعارات التيليجرام:</b>\n"
             "الاتصال بين راوتر ESP32-C5 Mini وحساب المشرف يعمل بكفاءة تامة! ✅");
    telegram_bot_send_alert(g_router_cfg.tg_admin_chat_id, test_msg);
}

/**
 * @brief Background task monitoring quota thresholds
 */
static void telegram_monitor_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Sleep 10s between checks

        if (!g_router_cfg.tg_enabled || strlen(g_router_cfg.tg_bot_token) == 0) continue;

        // 1. Check Main ISP Quota Multi-Thresholds
        if (g_router_cfg.isp_total_gb > 0.0f) {
            double std_div = (g_router_cfg.gb_standard == 1000) ? 1000000000.0 : 1073741824.0;
            double used_gb = (double)g_router_cfg.isp_used_bytes / std_div;
            double pct = (used_gb / (double)g_router_cfg.isp_total_gb) * 100.0;

            // Parse thresholds string "50,75,90,95"
            char thresh_copy[64];
            strncpy(thresh_copy, g_router_cfg.tg_thresholds, sizeof(thresh_copy) - 1);
            thresh_copy[sizeof(thresh_copy) - 1] = '\0';

            char *token = strtok(thresh_copy, ",");
            int t_idx = 0;
            while (token != NULL && t_idx < 16) {
                int t_val = atoi(token);
                if (t_val > 0 && t_val <= 100) {
                    if (pct >= (double)t_val && !(g_router_cfg.threshold_alerted_mask & (1 << t_idx))) {
                        g_router_cfg.threshold_alerted_mask |= (1 << t_idx);
                        storage_nvs_save_config();

                        char alert_msg[384];
                        snprintf(alert_msg, sizeof(alert_msg),
                                 "⚠️ <b>تنبيه استهلاك باقة الإنترنت (%d%%):</b>\n\n"
                                 "تم استهلاك %.2f GB من أصل %.1f GB.\n"
                                 "الرصيد المتبقي: %.2f GB.\n"
                                 "تاريخ التجديد القادم: %s.",
                                 t_val, used_gb, g_router_cfg.isp_total_gb,
                                 (g_router_cfg.isp_total_gb - used_gb),
                                 g_router_cfg.isp_cycle_renewal);
                        telegram_bot_send_alert(g_router_cfg.tg_admin_chat_id, alert_msg);
                    }
                }
                token = strtok(NULL, ",");
                t_idx++;
            }
        }

        // 2. Check Per-Device Quota Limits
        for (int i = 0; i < g_router_cfg.device_count; i++) {
            router_device_t *dev = &g_router_cfg.devices[i];
            if (dev->is_connected && dev->quota_gb > 0.0f) {
                double std_div = (g_router_cfg.gb_standard == 1000) ? 1000000000.0 : 1073741824.0;
                double dev_used = (double)(dev->rx_bytes + dev->tx_bytes) / std_div;
                if (dev_used >= (double)dev->quota_gb && !dev->quota_exceeded) {
                    dev->quota_exceeded = true;
                    char dev_msg[384];
                    snprintf(dev_msg, sizeof(dev_msg),
                             "🚫 <b>تم استهلاك كامل كوتة الجهاز:</b>\n\n"
                             "📱 <b>الجهاز:</b> %s (%s)\n"
                             "📊 <b>الاستهلاك:</b> %.2f / %.1f GB (100%%)\n"
                             "⚡ تم تطبيق الحظر التلقائي عبر الراوتر.",
                             dev->name, dev->ip, dev_used, dev->quota_gb);

                    telegram_bot_send_alert(g_router_cfg.tg_admin_chat_id, dev_msg);
                    if (strlen(dev->tg_chat_id) > 0) {
                        telegram_bot_send_alert(dev->tg_chat_id, dev_msg);
                    }
                }
            }
        }
    }
}

/**
 * @brief Long-Polling task querying Telegram getUpdates for interactive commands
 */
static void telegram_polling_task(void *pvParameters) {
    char url[256];
    char *rx_buffer = malloc(2048);
    if (!rx_buffer) {
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // 3 seconds interval

        if (!g_router_cfg.tg_enabled || strlen(g_router_cfg.tg_bot_token) == 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        snprintf(url, sizeof(url),
                 "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=5",
                 g_router_cfg.tg_bot_token, (long long)(s_last_update_id + 1));

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 8000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = esp_http_client_open(client, 0);

        if (err == ESP_OK) {
            int content_length = esp_http_client_fetch_headers(client);
            if (content_length > 0 && content_length < 2040) {
                int read_bytes = esp_http_client_read(client, rx_buffer, content_length);
                if (read_bytes > 0) {
                    rx_buffer[read_bytes] = '\0';

                    // Parse update_id
                    char *up_pos = strstr(rx_buffer, "\"update_id\":");
                    if (up_pos) {
                        int64_t up_id = strtoll(up_pos + 12, NULL, 10);
                        if (up_id > s_last_update_id) {
                            s_last_update_id = up_id;

                            // Extract chat id and text
                            char chat_id_str[32] = "";
                            char *chat_pos = strstr(rx_buffer, "\"chat\":{\"id\":");
                            if (chat_pos) {
                                chat_pos += 13;
                                int ci = 0;
                                while (*chat_pos && *chat_pos != ',' && *chat_pos != '}' && ci < 31) {
                                    chat_id_str[ci++] = *chat_pos++;
                                }
                                chat_id_str[ci] = '\0';
                            }

                            char cmd_text[128] = "";
                            char *text_pos = strstr(rx_buffer, "\"text\":\"");
                            if (text_pos) {
                                text_pos += 8;
                                int ti = 0;
                                while (*text_pos && *text_pos != '\"' && ti < 127) {
                                    cmd_text[ti++] = *text_pos++;
                                }
                                cmd_text[ti] = '\0';
                            }

                            if (strlen(cmd_text) > 0 && strlen(chat_id_str) > 0) {
                                ESP_LOGI(TAG, "Received Telegram command: '%s' from chat %s", cmd_text, chat_id_str);
                                char reply[1024];
                                telegram_execute_command(cmd_text, reply, sizeof(reply));
                                telegram_bot_send_alert(chat_id_str, reply);
                            }
                        }
                    }
                }
            }
        }
        esp_http_client_cleanup(client);
    }

    free(rx_buffer);
    vTaskDelete(NULL);
}

esp_err_t telegram_bot_init(void) {
    // Start background monitor task on Core 0 (Priority 1)
    xTaskCreate(telegram_monitor_task, "tg_monitor", 4096, NULL, 1, NULL);
    // Start background polling task on Core 0 (Priority 1)
    xTaskCreate(telegram_polling_task, "tg_polling", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "Telegram Bot subsystem initialized (Monitor & Long-Polling active)");
    return ESP_OK;
}
