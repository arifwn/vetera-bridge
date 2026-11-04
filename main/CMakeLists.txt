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
        "bookmarks.c"
        "dns_fwd.c"
        "debug_log.c"
        "sec_button.c"
        "http_proxy.c"

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
        esp_driver_gpio
)

# HTTP proxy redirect: hook lwIP's IPv4 input (see main/proxy_hook.h).
# ESP-IDF 5.4 has no Kconfig for this hook; lwip_default_hooks.h includes
# ESP_IDF_LWIP_HOOK_FILENAME instead, so set it on the lwip target only.
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_definitions(${lwip} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"proxy_hook.h\"")
target_include_directories(${lwip} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
