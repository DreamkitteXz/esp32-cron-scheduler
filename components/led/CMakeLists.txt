idf_component_register(SRCS "src/led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common
                       PRIV_REQUIRES esp_driver_gpio)
