idf_component_register(
    SRCS 
        "main.c"
        "wifi_5g_napt.c"
        "packet_shaper_iram.c"
        "storage_nvs.c"
        "web_server.c"
        "telegram_bot.c"
        "ota_manager.c"
        "time_reboot.c"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_wifi 
        esp_netif 
        lwip 
        nvs_flash 
        esp_http_server 
        esp_http_client 
        esp_https_ota 
        app_update 
        driver 
        esp_driver_gpio
        esp_pm 
        mbedtls
        esp_timer
    EMBED_FILES "web/index.html.gz"
)

