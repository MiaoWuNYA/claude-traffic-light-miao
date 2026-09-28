idf_component_register(SRCS "main.c" "mochi.c" "beep.c" "link_cmd.c" "ble_link.c"
                            "lv_font_sc14.c" "lv_font_sc12.c"
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_ledc
                                     esp_hw_support
                                     heap
                                     spi_flash
                                     nvs_flash
                                     lvgl
                                     app_update
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     lwip
                                     esp_http_server
                                     bt
                       INCLUDE_DIRS "..")
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
