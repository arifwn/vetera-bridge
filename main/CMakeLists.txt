idf_component_register(
    SRCS
        "main.c"
        "gateway.c"
        "ppp_link.c"
        "sdp_lap.c"
        "bt_bootstrap.c"
        "bt_name.c"
        "wifi_multi.c"
        "web_ui.c"
        "dns_fwd.c"

    INCLUDE_DIRS "."

    REQUIRES
        btstack-esp32
        esp_wifi
        esp_netif
        esp_http_server
        nvs_flash
        lwip
        freertos
        esp_timer
        esp_coex
)
