idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash
                       REQUIRES driver esp_event esp_netif lwip esp_timer esp_wifi nvs_flash  
                       INCLUDE_DIRS "")
