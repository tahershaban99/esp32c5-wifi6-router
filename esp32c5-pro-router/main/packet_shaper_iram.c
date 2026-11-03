/**
 * @file packet_shaper_iram.c
 * @brief High-Performance IRAM Packet Hooks, Token Bucket Rate Limiting, VIP QoS & Quota Drops.
 *
 * Implements low-latency, zero-jitter network packet management in IRAM:
 * - Token Bucket Algorithm for bidirectional bandwidth shaping (per-device Download & Upload caps)
 * - VIP Gaming Mode (DSCP 46 / Expedited Forwarding ToS priority tagging)
 * - Automatic Quota Enforcement (Immediate L3 drop when quota_gb is exceeded)
 * - Hard Device Blocking (Instant packet drop via lwIP input hook)
 * - Meter Scale & GB Standard Calibration
 * - Instantaneous Throughput Monitoring (Mbps / KB/s per client & WAN)
 */

#include "router_config.h"
#include <string.h>
#include <sys/param.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "lwip/ip4.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "esp_netif.h"

static const char *TAG = "packet_shaper";

static SemaphoreHandle_t s_device_mutex = NULL;
static netif_input_fn s_orig_ap_input  = NULL;
static netif_input_fn s_orig_sta_input = NULL;

extern esp_netif_t *s_ap_netif_handle;
extern esp_netif_t *s_sta_netif_handle;

/**
 * @brief Find device in registry by 6-byte MAC address
 */
router_device_t* router_get_device_by_mac(const uint8_t mac[6]) {
    for (int i = 0; i < g_router_cfg.device_count; i++) {
        if (memcmp(g_router_cfg.devices[i].mac, mac, 6) == 0) {
            return &g_router_cfg.devices[i];
        }
    }
    return NULL;
}

/**
 * @brief Find active connected device by IPv4 address string (e.g. "192.168.4.2")
 */
static router_device_t* router_get_device_by_ip_str(const char *ip_str) {
    if (!ip_str || ip_str[0] == '\0') return NULL;
    for (int i = 0; i < g_router_cfg.device_count; i++) {
        if (g_router_cfg.devices[i].is_connected &&
            strcmp(g_router_cfg.devices[i].ip, ip_str) == 0) {
            return &g_router_cfg.devices[i];
        }
    }
    return NULL;
}

/**
 * @brief Register or retrieve a client device upon association or DHCP lease
 */
router_device_t* router_get_or_create_device(const uint8_t mac[6], const char *ip, const char *hostname) {
    if (s_device_mutex) xSemaphoreTake(s_device_mutex, portMAX_DELAY);

    router_device_t *dev = router_get_device_by_mac(mac);
    if (!dev) {
        if (g_router_cfg.device_count < MAX_REGISTRY_DEVICES) {
            dev = &g_router_cfg.devices[g_router_cfg.device_count++];
            memset(dev, 0, sizeof(router_device_t));
            memcpy(dev->mac, mac, 6);
            snprintf(dev->mac_str, sizeof(dev->mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            if (hostname && strlen(hostname) > 0) {
                strncpy(dev->name, hostname, sizeof(dev->name) - 1);
            } else {
                snprintf(dev->name, sizeof(dev->name), "Device-%02x%02x", mac[4], mac[5]);
            }

            // Initial Token Bucket Capacity (100 KB burst budget)
            dev->tokens_down = 100000;
            dev->tokens_up   = 100000;
            dev->last_token_down_us = esp_timer_get_time();
            dev->last_token_up_us   = esp_timer_get_time();
        } else {
            // Registry full: replace oldest offline device entry
            int oldest_idx = -1;
            int64_t oldest_time = INT64_MAX;
            for (int i = 0; i < g_router_cfg.device_count; i++) {
                if (!g_router_cfg.devices[i].is_connected &&
                    g_router_cfg.devices[i].last_seen_epoch < oldest_time) {
                    oldest_time = g_router_cfg.devices[i].last_seen_epoch;
                    oldest_idx = i;
                }
            }
            if (oldest_idx >= 0) {
                dev = &g_router_cfg.devices[oldest_idx];
                memset(dev, 0, sizeof(router_device_t));
                memcpy(dev->mac, mac, 6);
                snprintf(dev->mac_str, sizeof(dev->mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

                if (hostname && strlen(hostname) > 0) {
                    strncpy(dev->name, hostname, sizeof(dev->name) - 1);
                } else {
                    snprintf(dev->name, sizeof(dev->name), "Device-%02x%02x", mac[4], mac[5]);
                }

                dev->tokens_down = 100000;
                dev->tokens_up   = 100000;
                dev->last_token_down_us = esp_timer_get_time();
                dev->last_token_up_us   = esp_timer_get_time();
            }
        }
    }

    if (dev) {
        dev->is_connected = true;
        if (ip && strlen(ip) > 0) {
            strncpy(dev->ip, ip, sizeof(dev->ip) - 1);
        }
        dev->last_seen_epoch = esp_timer_get_time() / 1000000LL;
    }

    if (s_device_mutex) xSemaphoreGive(s_device_mutex);
    return dev;
}

/**
 * @brief Reset cumulative byte usage counter for a specific device and lift quota block
 */
void router_reset_device_usage(const uint8_t mac[6]) {
    if (s_device_mutex) xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    router_device_t *dev = router_get_device_by_mac(mac);
    if (dev) {
        dev->rx_bytes = 0;
        dev->tx_bytes = 0;
        dev->prev_rx_bytes = 0;
        dev->prev_tx_bytes = 0;
        dev->quota_exceeded = false;
        dev->is_blocked = false;
        ESP_LOGI(TAG, "Reset byte usage and lifted quota block for device: %s (%s)", dev->name, dev->mac_str);
    }
    if (s_device_mutex) xSemaphoreGive(s_device_mutex);
    storage_nvs_save_config();
}

/**
 * @brief Remove an offline device from the historical registry
 */
void router_delete_offline_device(const uint8_t mac[6]) {
    if (s_device_mutex) xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    for (int i = 0; i < g_router_cfg.device_count; i++) {
        if (memcmp(g_router_cfg.devices[i].mac, mac, 6) == 0) {
            if (!g_router_cfg.devices[i].is_connected) {
                for (int j = i; j < g_router_cfg.device_count - 1; j++) {
                    g_router_cfg.devices[j] = g_router_cfg.devices[j + 1];
                }
                g_router_cfg.device_count--;
                ESP_LOGI(TAG, "Removed offline device from registry");
            }
            break;
        }
    }
    if (s_device_mutex) xSemaphoreGive(s_device_mutex);
    storage_nvs_save_config();
}

/**
 * @brief Clear all offline devices from the registry
 */
void router_clear_all_offline_devices(void) {
    if (s_device_mutex) xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    int write_idx = 0;
    for (int i = 0; i < g_router_cfg.device_count; i++) {
        if (g_router_cfg.devices[i].is_connected) {
            g_router_cfg.devices[write_idx++] = g_router_cfg.devices[i];
        }
    }
    g_router_cfg.device_count = write_idx;
    ESP_LOGI(TAG, "Cleared all offline history. Active devices retained: %d", write_idx);
    if (s_device_mutex) xSemaphoreGive(s_device_mutex);
    storage_nvs_save_config();
}

/**
 * @brief Fast IRAM Hook for SoftAP input packets (Upload traffic from clients to WAN)
 *
 * Runs entirely in IRAM for minimal latency.
 * - Drops packets if device is hard-blocked or has exceeded quota cap.
 * - Prioritizes gaming packets with ToS / DSCP 46 (Expedited Forwarding).
 * - Enforces upload Token Bucket bandwidth limit.
 */
static err_t IRAM_ATTR ap_input_hook(struct pbuf *p, struct netif *inp) {
    if (!p) return ERR_OK;

    // Check if IPv4 packet
    if (p->len >= sizeof(struct ip_hdr)) {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        if (IPH_V(iphdr) == 4) {
            char src_ip_str[16];
            ip4_addr_t src_ip;
            ip4_addr_copy(src_ip, iphdr->src);
            ip4addr_ntoa_r(&src_ip, src_ip_str, sizeof(src_ip_str));

            router_device_t *dev = router_get_device_by_ip_str(src_ip_str);
            if (dev) {
                // 1. Check Hard Block or Auto Quota Exceeded Drop
                if (dev->is_blocked || dev->quota_exceeded) {
                    pbuf_free(p);
                    return ERR_OK; // Packet dropped immediately at L3
                }

                uint16_t plen = p->tot_len;
                uint16_t scale = g_router_cfg.meter_scale_pct ? g_router_cfg.meter_scale_pct : 100;
                uint32_t scaled_bytes = (plen * scale) / 100;

                dev->tx_bytes += scaled_bytes;
                g_router_cfg.isp_used_bytes += scaled_bytes;

                // 2. Per-Device Quota Limit Check
                if (dev->quota_gb > 0.0f) {
                    double std_div = (g_router_cfg.gb_standard == 1000) ? 1000000000.0 : 1073741824.0;
                    double used_gb = (double)(dev->rx_bytes + dev->tx_bytes) / std_div;
                    if (used_gb >= (double)dev->quota_gb) {
                        dev->quota_exceeded = true;
                        pbuf_free(p);
                        return ERR_OK; // Immediate quota cut-off
                    }
                }

                // 3. Gaming VIP QoS: Set DSCP / ToS header to Expedited Forwarding (DSCP 46 = 0xB8)
                if (dev->is_gaming) {
                    IPH_TOS_SET(iphdr, 0xB8); // DSCP 46: Expedited Forwarding (Low Latency / Voice / Gaming)
                } else if (dev->up_cap_mbps > 0.0f) {
                    // 4. Token Bucket Rate Limiter for Upload
                    int64_t now_us = esp_timer_get_time();
                    int64_t elapsed_us = now_us - dev->last_token_up_us;
                    if (elapsed_us > 0) {
                        dev->last_token_up_us = now_us;
                        // 1 Mbps = 125,000 bytes/sec
                        int32_t add_tokens = (int32_t)((elapsed_us * (int64_t)(dev->up_cap_mbps * 125000.0f)) / 1000000LL);
                        dev->tokens_up = MIN(dev->tokens_up + add_tokens, (int32_t)(dev->up_cap_mbps * 25000.0f));
                    }
                    if (dev->tokens_up < (int32_t)plen) {
                        pbuf_free(p);
                        return ERR_OK; // Rate limited drop
                    }
                    dev->tokens_up -= plen;
                }
            }
        }
    }

    if (s_orig_ap_input) {
        return s_orig_ap_input(p, inp);
    }
    return ERR_OK;
}

/**
 * @brief Fast IRAM Hook for STA input packets (Download traffic from WAN to clients)
 *
 * Runs entirely in IRAM for maximum downstream throughput.
 * - Drops packets destined for blocked clients.
 * - Prioritizes gaming packets.
 * - Enforces download Token Bucket bandwidth limit.
 */
static err_t IRAM_ATTR sta_input_hook(struct pbuf *p, struct netif *inp) {
    if (!p) return ERR_OK;

    if (p->len >= sizeof(struct ip_hdr)) {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        if (IPH_V(iphdr) == 4) {
            char dst_ip_str[16];
            ip4_addr_t dst_ip;
            ip4_addr_copy(dst_ip, iphdr->dest);
            ip4addr_ntoa_r(&dst_ip, dst_ip_str, sizeof(dst_ip_str));

            router_device_t *dev = router_get_device_by_ip_str(dst_ip_str);
            if (dev) {
                // 1. Check Hard Block or Auto Quota Exceeded Drop
                if (dev->is_blocked || dev->quota_exceeded) {
                    pbuf_free(p);
                    return ERR_OK;
                }

                uint16_t plen = p->tot_len;
                uint16_t scale = g_router_cfg.meter_scale_pct ? g_router_cfg.meter_scale_pct : 100;
                uint32_t scaled_bytes = (plen * scale) / 100;

                dev->rx_bytes += scaled_bytes;
                g_router_cfg.isp_used_bytes += scaled_bytes;

                // 2. Per-Device Quota Limit Check
                if (dev->quota_gb > 0.0f) {
                    double std_div = (g_router_cfg.gb_standard == 1000) ? 1000000000.0 : 1073741824.0;
                    double used_gb = (double)(dev->rx_bytes + dev->tx_bytes) / std_div;
                    if (used_gb >= (double)dev->quota_gb) {
                        dev->quota_exceeded = true;
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }

                // 3. Gaming VIP QoS: Set DSCP / ToS header
                if (dev->is_gaming) {
                    IPH_TOS_SET(iphdr, 0xB8);
                } else if (dev->down_cap_mbps > 0.0f) {
                    // 4. Token Bucket Rate Limiter for Download
                    int64_t now_us = esp_timer_get_time();
                    int64_t elapsed_us = now_us - dev->last_token_down_us;
                    if (elapsed_us > 0) {
                        dev->last_token_down_us = now_us;
                        int32_t add_tokens = (int32_t)((elapsed_us * (int64_t)(dev->down_cap_mbps * 125000.0f)) / 1000000LL);
                        dev->tokens_down = MIN(dev->tokens_down + add_tokens, (int32_t)(dev->down_cap_mbps * 25000.0f));
                    }
                    if (dev->tokens_down < (int32_t)plen) {
                        pbuf_free(p);
                        return ERR_OK; // Rate limited drop
                    }
                    dev->tokens_down -= plen;
                }
            }
        }
    }

    if (s_orig_sta_input) {
        return s_orig_sta_input(p, inp);
    }
    return ERR_OK;
}

/**
 * @brief Calculate instantaneous download and upload speeds over a 2-second interval
 */
void packet_shaper_update_instant_speeds(void) {
    if (s_device_mutex) xSemaphoreTake(s_device_mutex, 50 / portTICK_PERIOD_MS);

    float total_down = 0.0f;
    float total_up   = 0.0f;

    for (int i = 0; i < g_router_cfg.device_count; i++) {
        router_device_t *dev = &g_router_cfg.devices[i];
        if (dev->is_connected) {
            uint64_t delta_rx = dev->rx_bytes >= dev->prev_rx_bytes ? (dev->rx_bytes - dev->prev_rx_bytes) : 0;
            uint64_t delta_tx = dev->tx_bytes >= dev->prev_tx_bytes ? (dev->tx_bytes - dev->prev_tx_bytes) : 0;

            dev->prev_rx_bytes = dev->rx_bytes;
            dev->prev_tx_bytes = dev->tx_bytes;

            // Speed in Mbps over 2 seconds: delta_bytes * 8 / (2.0 * 1,000,000)
            dev->down_mbps = (float)((delta_rx * 8.0) / 2000000.0);
            dev->up_mbps   = (float)((delta_tx * 8.0) / 2000000.0);

            total_down += dev->down_mbps;
            total_up   += dev->up_mbps;
        } else {
            dev->down_mbps = 0.0f;
            dev->up_mbps   = 0.0f;
        }
    }

    g_router_status.cur_down_mbps = total_down;
    g_router_status.cur_up_mbps   = total_up;

    if (s_device_mutex) xSemaphoreGive(s_device_mutex);
}

/**
 * @brief Hook into lwIP netif input functions for SoftAP and STA
 */
esp_err_t packet_shaper_init(void) {
    if (!s_device_mutex) {
        s_device_mutex = xSemaphoreCreateMutex();
    }

    // Bind hooks to lwIP netif list
    struct netif *nif = netif_list;
    while (nif != NULL) {
        if (nif->name[0] == 'a' && nif->name[1] == 'p') {
            s_orig_ap_input = nif->input;
            nif->input = ap_input_hook;
            ESP_LOGI(TAG, "Bound IRAM packet shaper hook to SoftAP netif");
        } else if (nif->name[0] == 's' && nif->name[1] == 't') {
            s_orig_sta_input = nif->input;
            nif->input = sta_input_hook;
            ESP_LOGI(TAG, "Bound IRAM packet shaper hook to STA netif");
        }
        nif = nif->next;
    }

    ESP_LOGI(TAG, "Packet Shaper & IRAM hooks initialized successfully");
    return ESP_OK;
}
