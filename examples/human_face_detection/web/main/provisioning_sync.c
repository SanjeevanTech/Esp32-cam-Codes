#include "provisioning_sync.h"
#include "device_config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "PROV_SYNC";
static char g_node_server_url[128] = {0};
static char g_bus_id[32] = {0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->user_data) {
            char *buffer = (char *)evt->user_data;
            int current_len = strlen(buffer);
            if (current_len + evt->data_len < 1024) {
                memcpy(buffer + current_len, evt->data, evt->data_len);
                buffer[current_len + evt->data_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static void provisioning_task(void *pvParameters) {
    ESP_LOGI(TAG, "🚀 Provisioning task ENTERED. Wait 10s for first check...");
    
    char *response_buffer = malloc(1024);
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // Wait 10 seconds for network to be fully ready on first boot, 
        // then 5 minutes between subsequent checks
        static bool first_check = true;
        if (first_check) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            first_check = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(300000));
        }
        
        ESP_LOGI(TAG, "🔍 Checking for updates at %s...", g_node_server_url);

        memset(response_buffer, 0, 1024);
        char url[256];
        snprintf(url, sizeof(url), "%s/api/device-config/get?bus_id=%s", g_node_server_url, g_bus_id);

        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = response_buffer,
            .timeout_ms = 5000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
            cJSON *root = cJSON_Parse(response_buffer);
            if (root) {
                cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
                cJSON *pass = cJSON_GetObjectItem(root, "wifi_password");
                cJSON *url_json = cJSON_GetObjectItem(root, "server_url");

                if (ssid && pass && url_json) {
                    device_config_t current_cfg;
                    device_config_load(&current_cfg);

                    bool changed = false;
                    if (strcmp(current_cfg.wifi_ssid, ssid->valuestring) != 0) changed = true;
                    if (strcmp(current_cfg.wifi_password, pass->valuestring) != 0) changed = true;
                    if (strcmp(current_cfg.server_url, url_json->valuestring) != 0) changed = true;

                    if (changed) {
                        ESP_LOGI(TAG, "🔄 New configuration detected! Saving and restarting...");
                        strncpy(current_cfg.wifi_ssid, ssid->valuestring, 31);
                        strncpy(current_cfg.wifi_password, pass->valuestring, 63);
                        strncpy(current_cfg.server_url, url_json->valuestring, 127);
                        
                        device_config_save(&current_cfg);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        esp_restart();
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGW(TAG, "❌ Failed to fetch updates: %s (Status: %d)", 
                     esp_err_to_name(err), esp_http_client_get_status_code(client));
            ESP_LOGI(TAG, "   Checking at: %s", url);
        }
        esp_http_client_cleanup(client);
    }
}

esp_err_t provisioning_sync_init(const char* node_server_url, const char* bus_id) {
    if (!node_server_url || !bus_id) return ESP_ERR_INVALID_ARG;
    strncpy(g_node_server_url, node_server_url, sizeof(g_node_server_url) - 1);
    strncpy(g_bus_id, bus_id, sizeof(g_bus_id) - 1);
    
    BaseType_t ret = xTaskCreatePinnedToCore(provisioning_task, "prov_sync", 4096, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create provisioning task! (No memory)");
        return ESP_FAIL;
    }
    return ESP_OK;
}
