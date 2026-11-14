include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event console esp_timer mbedtls
                  esp_app_format app_update esp_partition spi_flash fatfs vfs esp_http_client esp-tls tcp_transport lwip esp_driver_uart bt)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.

# The Retro emulator cores run every cycle of a game console: speed over size
# for these two files (the rest of the firmware stays -Os, sdkconfig.defaults).
set_source_files_properties(apps/gbemu/gbemu_app.c apps/gbemu/nes/agnes_build.c
                            PROPERTIES COMPILE_OPTIONS "-O2")
