idf_component_register(SRCS "ws_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_timer esp_http_client nvs_flash)