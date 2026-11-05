idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES radiolib oled scan_core ble_setup esp_driver_gpio esp_timer
                                     nvs_flash esp_wifi esp_netif esp_event esp_http_client
                                     mbedtls esp_hw_support)
