idf_component_register(
    SRCS
        "main.c"
        "max7219.c"
        "wifi_manager.c"
        "time_sync.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        lwip
)
