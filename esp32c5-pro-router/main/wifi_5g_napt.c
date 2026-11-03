/**
 * @file wifi_5g_napt.c
 * @brief ESP32-C5 Dual-Band (2.4GHz & 5GHz) Wi-Fi 6 NAPT Router, SoftAP & lwIP Forwarder.
 *
 * Implements native dual-band operation for Espressif ESP32-C5:
 * - 2.4 GHz (802.11b/g/n/ax Wi-Fi 6)
 * - 5 GHz (802.11a/n/ac/ax Wi-Fi 6)
 * - Hardware single-radio STA+AP channel synchronization
 * - lwIP IPv4 NAPT forwarding & DHCP DNS redirection
 * - Asynchronous dual-band active network scanning
 */

#include "router_config.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "wifi_5g_napt";

// Netif handles
esp_netif_t *s_sta_netif_handle = NULL;
esp_netif_t *s_ap_netif_handle  = NULL;

// Wi-Fi Scan Cache Structure & State
#define MAX_SCAN_RESULTS 24

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    char    band[8];        // "5G" or "2.4G"
    bool    secure;
    uint8_t authmode;
} wifi_scan_item_t;

static wifi_scan_item_t s_scan_cache[MAX_SCAN_RESULTS];
static uint16_t         s_scan_count = 0;
static SemaphoreHandle_t s_scan_mutex = NULL;
static bool             s_scan_in_progress = false;

/**
 * @brief Configure ESP32-C5 Band Mode, 802.11ax Wi-Fi 6 Protocols and Bandwidths.
 */
static void configure_c5_bands_and_protocols(void) {
    // 1. Configure Band Mode (Auto 2.4G+5G, 5GHz Only, or 2.4GHz Only)
    wifi_band_mode_t band_mode = WIFI_BAND_MODE_AUTO;
    if (g_router_cfg.ap_band_mode == ROUTER_BAND_MODE_5G_ONLY) {
        band_mode = WIFI_BAND_MODE_5G_ONLY;
        ESP_LOGI(TAG, "Configuring ESP32-C5 for 5 GHz ONLY operation");
    } else if (g_router_cfg.ap_band_mode == ROUTER_BAND_MODE_2G_ONLY) {
        band_mode = WIFI_BAND_MODE_2G_ONLY;
        ESP_LOGI(TAG, "Configuring ESP32-C5 for 2.4 GHz ONLY operation");
    } else {
        band_mode = WIFI_BAND_MODE_AUTO;
        ESP_LOGI(TAG, "Configuring ESP32-C5 for Dual-Band Auto (2.4GHz + 5GHz)");
    }

    esp_err_t err = esp_wifi_set_band_mode(band_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_band_mode returned: %s", esp_err_to_name(err));
    }

    // 2. Configure Supported Wi-Fi Protocols
    // ESP32-C5 supports 802.11ax (Wi-Fi 6) on both 2.4GHz and 5GHz
    wifi_protocols_t ap_protocols;
    memset(&ap_protocols, 0, sizeof(wifi_protocols_t));

    switch (g_router_cfg.ap_protocol) {
        case ROUTER_PROTO_AX_HE40:
            // Full Wi-Fi 6 (802.11ax) on both bands
            ap_protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
            ap_protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
            ESP_LOGI(TAG, "Wi-Fi Protocol: 802.11ax (Wi-Fi 6 HE40) enabled on 2.4GHz & 5GHz");
            break;

        case ROUTER_PROTO_AC_HT40:
            // 802.11a/n/ac Mixed on 5GHz, b/g/n on 2.4GHz
            ap_protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
            ap_protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC;
            ESP_LOGI(TAG, "Wi-Fi Protocol: 802.11a/n/ac Mixed (HT40/VHT40) enabled");
            break;

        case ROUTER_PROTO_N_HT20:
            // 802.11b/g/n HT20 for maximum compatibility
            ap_protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
            ap_protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
            ESP_LOGI(TAG, "Wi-Fi Protocol: 802.11b/g/n Mixed (HT20) enabled");
            break;

        case ROUTER_PROTO_N_HT40:
        default:
            // 802.11b/g/n/ax Mixed HT40
            ap_protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
            ap_protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
            ESP_LOGI(TAG, "Wi-Fi Protocol: 802.11b/g/n/ax Mixed (HT40) enabled");
            break;
    }

    err = esp_wifi_set_protocols(WIFI_IF_AP, &ap_protocols);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_protocols(AP) returned: %s", esp_err_to_name(err));
    }

    wifi_protocols_t sta_protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    esp_wifi_set_protocols(WIFI_IF_STA, &sta_protocols);

    // 3. Configure Channel Bandwidths (HT40/HE40)
    wifi_bandwidths_t bw = {
        .ghz_2g = (g_router_cfg.ap_protocol == ROUTER_PROTO_N_HT20) ? WIFI_BW20 : WIFI_BW40,
        .ghz_5g = (g_router_cfg.ap_protocol == ROUTER_PROTO_N_HT20) ? WIFI_BW20 : WIFI_BW40,
    };
    err = esp_wifi_set_bandwidths(WIFI_IF_AP, &bw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_bandwidths(AP) returned: %s", esp_err_to_name(err));
    }
    esp_wifi_set_bandwidths(WIFI_IF_STA, &bw);
}

/**
 * @brief Wi-Fi and IP Stack Event Handler
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Wi-Fi STA started. Initiating connection to upstream AP: %s...", g_router_cfg.sta_ssid);
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                g_router_status.sta_channel = event->channel;

                // Detect frequency band based on channel number
                if (event->channel > 14) {
                    strncpy(g_router_status.sta_band_str, "5GHz", sizeof(g_router_status.sta_band_str) - 1);
                } else {
                    strncpy(g_router_status.sta_band_str, "2.4GHz", sizeof(g_router_status.sta_band_str) - 1);
                }

                ESP_LOGI(TAG, "STA Connected to '%s' on Channel %d (%s)!",
                         event->ssid, event->channel, g_router_status.sta_band_str);

                // Hardware Single-Radio Constraint:
                // SoftAP must operate on the exact same radio channel as the connected STA.
                // Log and synchronize SoftAP channel state.
                ESP_LOGI(TAG, "Synchronizing SoftAP radio channel to match Upstream STA (Ch: %d, %s)",
                         event->channel, g_router_status.sta_band_str);
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "STA disconnected from parent AP (Reason: %d). Reconnecting in 2.5s...", disc->reason);
                g_router_status.sta_connected = false;

                // Revert SoftAP channel back to user-configured standalone default if needed
                vTaskDelay(pdMS_TO_TICKS(2500));
                esp_wifi_connect();
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "SoftAP Client Associated: MAC %02x:%02x:%02x:%02x:%02x:%02x (AID=%d)",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5], event->aid);

                // Register client into device table
                router_get_or_create_device(event->mac, "", "");
                g_router_status.ap_clients_count++;
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "SoftAP Client Left: MAC %02x:%02x:%02x:%02x:%02x:%02x",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5]);

                router_device_t *dev = router_get_device_by_mac(event->mac);
                if (dev) {
                    dev->is_connected = false;
                    dev->last_seen_epoch = esp_timer_get_time() / 1000000LL;
                }
                if (g_router_status.ap_clients_count > 0) {
                    g_router_status.ap_clients_count--;
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                g_router_status.sta_connected = true;
                strncpy(g_router_status.sta_active_ssid, g_router_cfg.sta_ssid, sizeof(g_router_status.sta_active_ssid) - 1);

                esp_ip4addr_ntoa(&event->ip_info.ip, g_router_status.sta_ip, sizeof(g_router_status.sta_ip));
                esp_ip4addr_ntoa(&event->ip_info.gw, g_router_status.sta_gw, sizeof(g_router_status.sta_gw));
                esp_ip4addr_ntoa(&event->ip_info.netmask, g_router_status.sta_mask, sizeof(g_router_status.sta_mask));

                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    g_router_status.sta_rssi    = ap_info.rssi;
                    g_router_status.sta_channel = ap_info.primary;
                    if (ap_info.primary > 14) {
                        strncpy(g_router_status.sta_band_str, "5GHz", sizeof(g_router_status.sta_band_str) - 1);
                    } else {
                        strncpy(g_router_status.sta_band_str, "2.4GHz", sizeof(g_router_status.sta_band_str) - 1);
                    }
                }

                ESP_LOGI(TAG, "WAN Online! IP: %s, GW: %s, Band: %s (Ch %d, RSSI %d dBm)",
                         g_router_status.sta_ip, g_router_status.sta_gw,
                         g_router_status.sta_band_str, g_router_status.sta_channel, g_router_status.sta_rssi);

                // Enable lwIP IPv4 NAPT on the SoftAP subnet
                esp_netif_ip_info_t ap_ip_info;
                esp_netif_get_ip_info(s_ap_netif_handle, &ap_ip_info);
                ip_napt_enable(ap_ip_info.ip.addr, 1);
                ESP_LOGI(TAG, "lwIP IPv4 NAPT forwarding activated on SoftAP gateway (%s) -> WAN (%s)",
                         DEFAULT_AP_IP, g_router_status.sta_ip);

                // Configure DNS server forwarding for connected SoftAP clients
                esp_netif_dns_info_t upstream_dns;
                if (esp_netif_get_dns_info(s_sta_netif_handle, ESP_NETIF_DNS_MAIN, &upstream_dns) == ESP_OK) {
                    esp_ip4addr_ntoa(&upstream_dns.ip.u_addr.ip4, g_router_status.sta_dns, sizeof(g_router_status.sta_dns));
                    esp_netif_set_dns_info(s_ap_netif_handle, ESP_NETIF_DNS_MAIN, &upstream_dns);
                    ESP_LOGI(TAG, "SoftAP DHCP DNS redirected to upstream server: %s", g_router_status.sta_dns);
                }

                // Ensure packet shaper netif hooks are bound
                packet_shaper_init();
                break;
            }

            case IP_EVENT_AP_STAIPASSIGNED: {
                ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
                char client_ip_str[16];
                esp_ip4addr_ntoa(&event->ip, client_ip_str, sizeof(client_ip_str));
                ESP_LOGI(TAG, "DHCP leased IP %s to MAC %02x:%02x:%02x:%02x:%02x:%02x",
                         client_ip_str, event->mac[0], event->mac[1],
                         event->mac[2], event->mac[3], event->mac[4], event->mac[5]);

                router_get_or_create_device(event->mac, client_ip_str, "");
                break;
            }

            default:
                break;
        }
    }
}

/**
 * @brief Initialize ESP32-C5 Dual-Band Wi-Fi 6 Stack, Netifs, SoftAP & NAPT
 */
esp_err_t wifi_5g_napt_init(void) {
    if (!s_scan_mutex) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. Create STA and SoftAP network interfaces
    s_sta_netif_handle = esp_netif_create_default_wifi_sta();
    s_ap_netif_handle  = esp_netif_create_default_wifi_ap();

    // 2. Set SoftAP IP Configuration & DHCP Lease Range (192.168.4.1/24)
    esp_netif_ip_info_t ap_ip;
    memset(&ap_ip, 0, sizeof(ap_ip));
    ip4addr_aton(DEFAULT_AP_IP, &ap_ip.ip);
    ip4addr_aton(DEFAULT_AP_IP, &ap_ip.gw);
    ip4addr_aton(DEFAULT_AP_NETMASK, &ap_ip.netmask);

    esp_netif_dhcps_stop(s_ap_netif_handle);
    esp_netif_set_ip_info(s_ap_netif_handle, &ap_ip);

    dhcps_lease_t lease;
    lease.enable = true;
    ip4addr_aton("192.168.4.2", &lease.start_ip);
    ip4addr_aton("192.168.4.20", &lease.end_ip);
    esp_netif_dhcps_option(s_ap_netif_handle, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                           &lease, sizeof(dhcps_lease_t));
    esp_netif_dhcps_start(s_ap_netif_handle);

    // 3. Configure STA Static IP if requested
    if (g_router_cfg.sta_is_static && strlen(g_router_cfg.sta_ip) > 0) {
        esp_netif_dhcpc_stop(s_sta_netif_handle);
        esp_netif_ip_info_t sta_ip;
        memset(&sta_ip, 0, sizeof(sta_ip));
        ip4addr_aton(g_router_cfg.sta_ip, &sta_ip.ip);
        ip4addr_aton(g_router_cfg.sta_gw, &sta_ip.gw);
        ip4addr_aton(g_router_cfg.sta_mask, &sta_ip.netmask);
        esp_netif_set_ip_info(s_sta_netif_handle, &sta_ip);

        esp_netif_dns_info_t dns_info;
        ip4addr_aton(g_router_cfg.sta_dns, &dns_info.ip.u_addr.ip4);
        dns_info.ip.type = IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif_handle, ESP_NETIF_DNS_MAIN, &dns_info);
        ESP_LOGI(TAG, "Static IP configured: IP=%s, GW=%s, Mask=%s, DNS=%s",
                 g_router_cfg.sta_ip, g_router_cfg.sta_gw, g_router_cfg.sta_mask, g_router_cfg.sta_dns);
    }

    // 4. Initialize Wi-Fi Driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // 6. Set Wi-Fi Mode to APSTA (Concurrent SoftAP and Station)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 7. Apply Dual-Band 5GHz & Wi-Fi 6 Protocols
    configure_c5_bands_and_protocols();

    // 8. Configure STA Target
    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strncpy((char *)sta_config.sta.ssid, g_router_cfg.sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, g_router_cfg.sta_password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // 9. Configure SoftAP Broadcast
    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ap.ssid, g_router_cfg.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, g_router_cfg.ap_password, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(g_router_cfg.ap_ssid);
    ap_config.ap.channel = g_router_cfg.ap_channel ? g_router_cfg.ap_channel : 36;
    ap_config.ap.max_connection = MAX_CONNECTED_DEVICES;
    ap_config.ap.authmode = (strlen(g_router_cfg.ap_password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // 10. Power Save Mode
    esp_wifi_set_ps(g_router_cfg.wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);

    // 11. Start Wi-Fi Hardware
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "ESP32-C5 Dual-Band Wi-Fi 6 Router Subsystem initialized and operational");

    return ESP_OK;
}

/**
 * @brief Dynamic Reconfiguration of Wi-Fi parameters without reboot
 */
esp_err_t wifi_5g_napt_reconfigure(void) {
    ESP_LOGI(TAG, "Applying Wi-Fi reconfiguration...");
    configure_c5_bands_and_protocols();

    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strncpy((char *)sta_config.sta.ssid, g_router_cfg.sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, g_router_cfg.sta_password, sizeof(sta_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ap.ssid, g_router_cfg.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, g_router_cfg.ap_password, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(g_router_cfg.ap_ssid);
    ap_config.ap.channel = g_router_cfg.ap_channel ? g_router_cfg.ap_channel : 36;
    ap_config.ap.max_connection = MAX_CONNECTED_DEVICES;
    ap_config.ap.authmode = (strlen(g_router_cfg.ap_password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    esp_wifi_set_ps(g_router_cfg.wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);

    esp_wifi_disconnect();
    esp_wifi_connect();
    return ESP_OK;
}

/**
 * @brief Dual-Band Wi-Fi Scan Task (2.4GHz & 5GHz)
 */
static void wifi_scan_task(void *pvParameters) {
    s_scan_in_progress = true;
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,               // 0 = Scan all channels across active bands
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_LOGI(TAG, "Starting active Wi-Fi scan across 2.4GHz and 5GHz bands...");
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_OK) {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > 0) {
            wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_num);
            if (ap_records) {
                esp_wifi_scan_get_ap_records(&ap_num, ap_records);

                if (s_scan_mutex) xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                s_scan_count = 0;
                for (int i = 0; i < ap_num && s_scan_count < MAX_SCAN_RESULTS; i++) {
                    if (strlen((char *)ap_records[i].ssid) == 0) continue;

                    wifi_scan_item_t *item = &s_scan_cache[s_scan_count++];
                    strncpy(item->ssid, (char *)ap_records[i].ssid, sizeof(item->ssid) - 1);
                    item->rssi = ap_records[i].rssi;
                    item->channel = ap_records[i].primary;
                    item->authmode = ap_records[i].authmode;
                    item->secure = (ap_records[i].authmode != WIFI_AUTH_OPEN);

                    // Categorize Band (5GHz vs 2.4GHz)
                    if (ap_records[i].primary > 14) {
                        strncpy(item->band, "5G", sizeof(item->band) - 1);
                    } else {
                        strncpy(item->band, "2.4G", sizeof(item->band) - 1);
                    }
                }
                if (s_scan_mutex) xSemaphoreGive(s_scan_mutex);

                free(ap_records);
                ESP_LOGI(TAG, "Wi-Fi scan completed: found %d access points across 2.4GHz & 5GHz", s_scan_count);
            }
        }
    } else {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    }
    s_scan_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t wifi_5g_napt_start_scan(void) {
    if (s_scan_in_progress) return ESP_ERR_INVALID_STATE;
    xTaskCreate(wifi_scan_task, "scan_task", 4096, NULL, 2, NULL);
    return ESP_OK;
}
