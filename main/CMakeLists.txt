idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "api_server.c"
        "ws_server.c"
        "tt_config_api.c"
        "renfe_client.c"
        "renfe_parser.c"
        "gtfs_index.c"
        "renfe_arrivals.c"
        "renfe_state.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_driver_usb_serial_jtag
        esp_http_server
        esp_http_client
        mbedtls
        espressif__cjson
    EMBED_FILES "pozuelo_gtfs.bin"
)
