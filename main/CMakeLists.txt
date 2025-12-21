# Embed the server root certificate into the final binary
idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "rfid.c" "main.c" "network.c" "netconsole.c" "ota.c" "frame.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES app_update esp_eth esp_driver_gpio driver esp_driver_spi
                                  esp_netif nvs_flash esp_psram ethernet_init)

