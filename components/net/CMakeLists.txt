idf_component_register(SRCS "src/net.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash)
