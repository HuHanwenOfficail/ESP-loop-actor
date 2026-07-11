idf_component_register(
    SRCS "esp-loop-actor.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_tinyusb
        tinyusb
        driver
        nvs_flash
        spiffs
        json
        esp_http_server
        esp_wifi
        esp_netif
)