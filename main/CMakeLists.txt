
idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "api_server.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_driver_usb_serial_jtag
        esp_http_server
)
