set(SRCS
    "main.c" "csi_collector.c" "stream_sender.c" "nvs_config.c"
    "edge_processing.c" "ota_update.c" "power_mgmt.c"
    "wasm_runtime.c" "wasm_upload.c" "rvf_parser.c"
    "mmwave_sensor.c"
)

# Explicit component dependencies for the main component. (json = cJSON for the
# OTA /config endpoint; the rest are what main's sources #include.)
set(REQUIRES
    nvs_flash esp_wifi esp_http_server app_update esp_netif
    mbedtls driver esp_timer esp_adc esp_hw_support json
)

# ADR-061: Mock CSI generator for QEMU testing
if(CONFIG_CSI_MOCK_ENABLED)
    list(APPEND SRCS "mock_csi.c")
endif()

# ADR-045: Display support (compile-time optional)
# S3: SH8601 AMOLED + FT3168 touch (needs esp_lcd_touch)
# C6: ST7789 SPI TFT (no touch component needed)
if(CONFIG_DISPLAY_ENABLE)
    list(APPEND SRCS "display_hal.c" "display_ui.c" "display_task.c")
    if(CONFIG_IDF_TARGET_ESP32C6)
        list(APPEND REQUIRES esp_lcd lvgl)
    else()
        list(APPEND REQUIRES esp_lcd esp_lcd_touch lvgl)
    endif()
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
