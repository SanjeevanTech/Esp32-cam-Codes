/**
 * @file board_heartbeat.c
 * @brief ESP32 Board Heartbeat Implementation
 */

#include "board_heartbeat.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include <string.h>

static const char* TAG = "HEARTBEAT";

// Configuration
static char g_server_url[128] = {0};
static char g_bus_id[32] = {0};
static char g_device_id[64] = {0};
static char g_location[64] = {0};

// Task handle
static TaskHandle_t heartbeat_task_handle = NULL;
static bool heartbeat_running = false;

// Heartbeat interval (60 seconds)
#define HEARTBEAT_INTERVAL_MS (60 * 1000)

/**
 * @brief Get local IP address as string
 */
static bool get_local_ip(char* ip_str, size_t max_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG, "WiFi interface not found");
        return false;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get IP info");
        return false;
    }
    
    snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

/**
 * @brief Send heartbeat to Python server
 */
static esp_err_t send_heartbeat(void)
{
    char url[256];
    char ip_address[16] = "unknown";
    
    // Get local IP
    get_local_ip(ip_address, sizeof(ip_address));
    
    // Build URL
    snprintf(url, sizeof(url), "%s/api/board-heartbeat", g_server_url);
    
    // Create JSON payload
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(root, "bus_id", g_bus_id);
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddStringToObject(root, "location", g_location);
    cJSON_AddStringToObject(root, "ip_address", ip_address);
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to print JSON");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle for HTTPS
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        cJSON_Delete(root);
        free(json_str);
        return ESP_FAIL;
    }
    
    // Set headers and body
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));
    
    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            ESP_LOGI(TAG, "💓 Heartbeat sent: %s @ %s", g_device_id, ip_address);
        } else {
            ESP_LOGW(TAG, "Heartbeat failed: HTTP %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "Heartbeat error: %s", esp_err_to_name(err));
    }
    
    // Cleanup
    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    free(json_str);
    
    return err;
}

/**
 * @brief Heartbeat task - runs every 60 seconds
 */
static void heartbeat_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Heartbeat task started");
    ESP_LOGI(TAG, "  Server: %s", g_server_url);
    ESP_LOGI(TAG, "  Bus: %s", g_bus_id);
    ESP_LOGI(TAG, "  Device: %s", g_device_id);
    ESP_LOGI(TAG, "  Location: %s", g_location);
    ESP_LOGI(TAG, "  Interval: %d seconds", HEARTBEAT_INTERVAL_MS / 1000);
    
    // Wait 10 seconds before first heartbeat (let WiFi stabilize)
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    while (heartbeat_running) {
        // Send heartbeat
        esp_err_t err = send_heartbeat();
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Heartbeat failed, will retry in 60s");
        }
        
        // Wait for next interval
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
    
    ESP_LOGI(TAG, "Heartbeat task stopped");
    heartbeat_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Initialize heartbeat system
 */
esp_err_t board_heartbeat_init(const char* server_url, const char* bus_id, 
                               const char* device_id, const char* location)
{
    if (server_url == NULL || bus_id == NULL || device_id == NULL || location == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Store configuration
    strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);
    strncpy(g_bus_id, bus_id, sizeof(g_bus_id) - 1);
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    strncpy(g_location, location, sizeof(g_location) - 1);
    
    ESP_LOGI(TAG, "Heartbeat initialized");
    return ESP_OK;
}

/**
 * @brief Start heartbeat task
 */
esp_err_t board_heartbeat_start(void)
{
    if (heartbeat_task_handle != NULL) {
        ESP_LOGW(TAG, "Heartbeat task already running");
        return ESP_OK;
    }
    
    if (strlen(g_server_url) == 0) {
        ESP_LOGE(TAG, "Heartbeat not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    heartbeat_running = true;
    
    BaseType_t ret = xTaskCreate(
        heartbeat_task,
        "heartbeat",
        6144,
        NULL,
        5,
        &heartbeat_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
        heartbeat_running = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Heartbeat task started");
    return ESP_OK;
}
