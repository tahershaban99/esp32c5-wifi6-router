/**
 * @file router_config.h
 * @brief Global data structures and configurations for ESP32-C5 Dual-Band Wi-Fi 6 Router.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTER_FIRMWARE_VERSION     "v3.2.0-PRO"
#define ROUTER_HARDWARE_TARGET      "ESP32-C5 Mini (5GHz Wi-Fi 6)"

#define MAX_CONNECTED_DEVICES       12
#define MAX_REGISTRY_DEVICES        32

#define DEFAULT_CPU_FREQ_MHZ        240
#define DEFAULT_ADMIN_USER          "admin"
#define DEFAULT_ADMIN_PASS          "admin"
#define DEFAULT_AP_SSID             "ESP32-C5-Router"
#define DEFAULT_AP_PASS             "12345678"
#define DEFAULT_AP_IP               "192.168.4.1"
#define DEFAULT_AP_NETMASK          "255.255.255.0"
#define ROUTER_CFG_MAGIC            0xC502

#define GPIO_FACTORY_RESET_PIN      9    // BOOT button on ESP32-C5
#define FACTORY_RESET_HOLD_MS       10000

// Wi-Fi Band Modes for ESP32-C5
enum {
    ROUTER_BAND_MODE_2G_ONLY = 1,
    ROUTER_BAND_MODE_5G_ONLY = 2,
    ROUTER_BAND_MODE_AUTO    = 3
};

// Wi-Fi Protocol & Bandwidth Modes
enum {
    ROUTER_PROTO_N_HT20     = 0,   // 802.11b/g/n Mixed (HT20)
    ROUTER_PROTO_N_HT40     = 1,   // 802.11b/g/n/ax Mixed (HT40)
    ROUTER_PROTO_N_ONLY_HT40= 2,   // 802.11n Only (HT40)
    ROUTER_PROTO_N_ONLY_HT20= 3,   // 802.11n Only (HT20)
    ROUTER_PROTO_AX_HE40    = 4,   // 802.11ax (Wi-Fi 6 HE40)
    ROUTER_PROTO_AC_HT40    = 5    // 802.11a/n/ac/ax Mixed (5GHz HT40/VHT40)
};

/**
 * @brief Individual Client / Device entry
 */
typedef struct {
    uint8_t  mac[6];
    char     mac_str[18];
    char     ip[16];
    char     name[33];
    bool     is_connected;
    int64_t  last_seen_epoch;

    // Instantaneous Speeds (calculated every 2 seconds)
    float    down_mbps;
    float    up_mbps;

    // Cumulative Byte Counters (raw bytes)
    uint64_t rx_bytes;              // Downloaded by device
    uint64_t tx_bytes;              // Uploaded by device
    uint64_t prev_rx_bytes;
    uint64_t prev_tx_bytes;

    // Control parameters
    float    quota_gb;              // 0.0 = unlimited
    float    down_cap_mbps;         // 0.0 = unlimited
    float    up_cap_mbps;           // 0.0 = unlimited
    char     tg_chat_id[32];        // Dedicated Telegram Chat ID for alerts
    bool     is_gaming;             // VIP Gaming Mode (High Priority QoS DSCP)
    bool     is_blocked;            // Drop L3 packets
    bool     quota_exceeded;        // Auto-dropped when rx+tx >= quota_gb

    // Token Bucket Rate Limiting (enforced in IRAM)
    int64_t  last_token_down_us;
    int64_t  last_token_up_us;
    int32_t  tokens_down;
    int32_t  tokens_up;
} router_device_t;

/**
 * @brief Global Router Persistent Configuration (stored in NVS)
 */
typedef struct {
    uint32_t magic;
    // Upstream Wi-Fi (STA)
    char     sta_ssid[33];
    char     sta_password[64];
    bool     sta_is_static;
    char     sta_ip[16];
    char     sta_mask[16];
    char     sta_gw[16];
    char     sta_dns[16];

    // ESP32-C5 Broadcast (SoftAP)
    char     ap_ssid[33];
    char     ap_password[64];
    uint8_t  ap_channel;
    uint8_t  ap_band_mode;          // AUTO, 5G_ONLY, 2G_ONLY
    uint8_t  ap_protocol;           // HE40, HT40, etc.

    // RISC-V CPU Performance & Power
    uint16_t cpu_freq_mhz;          // 240, 160, 80
    bool     wifi_power_save;

    // Main ISP Quota & Meter Calibration
    float    isp_total_gb;
    uint64_t isp_used_bytes;
    char     isp_cycle_start[16];
    char     isp_cycle_renewal[16];
    uint16_t meter_scale_pct;       // 50% to 110%, default 100
    uint16_t gb_standard;          // 1024 (Binary) or 1000 (Decimal)

    // Telegram Bot & Multi-Threshold Alerts
    char     tg_bot_token[80];
    char     tg_admin_chat_id[32];
    char     tg_thresholds[64];     // e.g. "50,75,90,95"
    bool     tg_enabled;
    uint32_t threshold_alerted_mask;

    // Daily Scheduled Auto-Reboot & Timezone
    bool     reboot_enabled;
    char     reboot_time[8];        // "HH:MM" e.g. "03:00"
    int8_t   tz_offset_hours;       // UTC offset (-12 to +14)

    // Admin Credentials
    char     admin_user[33];
    char     admin_pass[65];

    // Offline & Known Device Registry
    uint16_t device_count;
    router_device_t devices[MAX_REGISTRY_DEVICES];
} router_global_config_t;

/**
 * @brief Live Operational Metrics & Status
 */
typedef struct {
    bool     sta_connected;
    char     sta_active_ssid[33];
    int8_t   sta_rssi;
    uint8_t  sta_channel;
    char     sta_band_str[8];       // "5GHz" or "2.4GHz"
    char     sta_ip[16];
    char     sta_gw[16];
    char     sta_dns[16];
    char     sta_mask[16];

    float    cur_down_mbps;
    float    cur_up_mbps;

    uint32_t free_heap_kb;
    uint32_t min_heap_kb;
    uint32_t total_heap_kb;

    int64_t  uptime_seconds;
    bool     ntp_synced;
    char     current_time_str[32];

    uint8_t  ap_clients_count;
    int64_t  countdown_reboot_target_epoch; // 0 if none
} router_live_status_t;

// Global Accessors
extern router_global_config_t g_router_cfg;
extern router_live_status_t   g_router_status;

// API Declarations
esp_err_t storage_nvs_init(void);
esp_err_t storage_nvs_load_config(void);
esp_err_t storage_nvs_save_config(void);
esp_err_t storage_nvs_save_counters(void);
void      storage_nvs_reset_admin_credentials(void);

esp_err_t wifi_5g_napt_init(void);
esp_err_t wifi_5g_napt_start_scan(void);
esp_err_t wifi_5g_napt_reconfigure(void);

esp_err_t packet_shaper_init(void);
void      packet_shaper_update_instant_speeds(void);
router_device_t* router_get_device_by_mac(const uint8_t mac[6]);
router_device_t* router_get_or_create_device(const uint8_t mac[6], const char *ip, const char *hostname);
void      router_reset_device_usage(const uint8_t mac[6]);
void      router_delete_offline_device(const uint8_t mac[6]);
void      router_clear_all_offline_devices(void);

esp_err_t time_reboot_init(void);
void      time_reboot_sync_sntp(void);
void      time_reboot_set_countdown(int minutes);
void      time_reboot_trigger_immediate(void);

esp_err_t telegram_bot_init(void);
esp_err_t telegram_bot_send_alert(const char *chat_id, const char *markdown_msg);
void      telegram_bot_test_alert(void);
void      telegram_execute_command(const char *cmd, char *out_buf, size_t out_max);

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

esp_err_t ota_manager_init(void);
esp_err_t ota_manager_start_url(const char *url);
void      ota_manager_get_info(char *cur_part, char *next_part, bool *rollback_valid);

#ifdef __cplusplus
}
#endif

