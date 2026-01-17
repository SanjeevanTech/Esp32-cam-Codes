idf_component_register(SRCS "app_main.cpp" "device_config.c" "esp32_power_management_fixed.c" "board_heartbeat.c" "provisioning_sync.c"
                       REQUIRES modules storage lwip nvs_flash esp_http_client json esp-tls)
