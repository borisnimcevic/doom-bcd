idf_component_register(SRCS "doomgeneric_bcd.c"
                       INCLUDE_DIRS "."
                       REQUIRES doomgeneric
                                esp_lcd
                                esp_driver_spi
                                esp_driver_gpio
                                esp_driver_rmt
                                esp_timer
                                esp_psram
                                spiffs)
