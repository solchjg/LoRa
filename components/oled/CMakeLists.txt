# A 128x64 SSD1306 status screen for the Heltec programs (firmware/scanner,
# firmware/survey): five lines of 21 characters in Terminus 6x12.
idf_component_register(SRCS "oled.c" "font6x12.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES text esp_lcd esp_driver_i2c)
