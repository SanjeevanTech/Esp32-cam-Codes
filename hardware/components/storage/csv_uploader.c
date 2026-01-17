#include "csv_uploader.h"
#include "csv_logger.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include "mbedtls/base64.h"

static const char* TAG = "CSV_UPLOADER";

static csv_uploader_config_t s_config;
static TaskHandle_t s_upload_task = NULL;
static SemaphoreHandle_t s_trigger_sem = NULL;
static bool s_running = false;

// Enhanced error handling and offline buffering
static csv_uploader_status_t s_status = {0};
static face_log_entry_t* s_offline_buffer = NULL;
static int s_offline_buffer_count = 0;
static SemaphoreHandle_t s_status_mutex = NULL;

// Forward declaration
static esp_err_t upload_logs_to_server(face_log_entry_t* logs, int count);

// Calculate exponential backoff delay
static int calculate_backoff_delay(int attempt) {
    int base_delay = s_config.retry_backoff_base_ms;
    int delay = base_delay * (1 << attempt); // Exponential backoff: base * 2^attempt
    
    // Cap at maximum delay
    if (delay > s_config.max_retry_delay_ms) {
        delay = s_config.max_retry_delay_ms;
    }
    
    return delay;
}

// Update status with error information
static void update_status_error(const char* error_msg) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.is_online = false;
        s_status.failed_uploads++;
        s_status.consecutive_failures++;
        strncpy(s_status.last_error, error_msg, sizeof(s_status.last_error) - 1);
        s_status.last_error[sizeof(s_status.last_error) - 1] = '\0';
        xSemaphoreGive(s_status_mutex);
    }
}

// Update status with success information
static void update_status_success(int uploaded_count) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.is_online = true;
        s_status.successful_uploads += uploaded_count;
        s_status.consecutive_failures = 0;
        s_status.last_successful_upload_time = esp_timer_get_time();
        strcpy(s_status.last_error, "");
        xSemaphoreGive(s_status_mutex);
    }
}

// Add logs to offline buffer
static esp_err_t add_to_offline_buffer(face_log_entry_t* logs, int count) {
    if (!s_config.enable_offline_buffering || !s_offline_buffer) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        int space_available = s_config.offline_buffer_size - s_offline_buffer_count;
        int to_add = (count > space_available) ? space_available : count;
        
        if (to_add > 0) {
            memcpy(&s_offline_buffer[s_offline_buffer_count], logs, to_add * sizeof(face_log_entry_t));
            s_offline_buffer_count += to_add;
            s_status.offline_buffer_count = s_offline_buffer_count;
            
            ESP_LOGI(TAG, "Added %d entries to offline buffer (%d/%d)", 
                     to_add, s_offline_buffer_count, s_config.offline_buffer_size);
        }
        
        if (to_add < count) {
            ESP_LOGW(TAG, "Offline buffer full, dropped %d entries", count - to_add);
        }
        
        xSemaphoreGive(s_status_mutex);
        return (to_add > 0) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    
    return ESP_ERR_TIMEOUT;
}

// Try to upload offline buffer
static esp_err_t upload_offline_buffer(void) {
    if (!s_config.enable_offline_buffering || !s_offline_buffer || s_offline_buffer_count == 0) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Attempting to upload %d entries from offline buffer", s_offline_buffer_count);
    
    esp_err_t result = upload_logs_to_server(s_offline_buffer, s_offline_buffer_count);
    if (result == ESP_OK) {
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_offline_buffer_count = 0;
            s_status.offline_buffer_count = 0;
            xSemaphoreGive(s_status_mutex);
        }
        ESP_LOGI(TAG, "Successfully uploaded offline buffer");
    }
    
    return result;
}

static esp_err_t upload_logs_to_server(face_log_entry_t* logs, int count)
{
    if (count == 0) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Uploading %d log entries to server", count);
    
    // Create JSON payload
    cJSON* root = cJSON_CreateObject();
    cJSON* device_id = cJSON_CreateString(logs[0].device_id);
    cJSON* bus_id = cJSON_CreateString(logs[0].bus_id);  // MULTI-BUS: Add bus_id to request
    cJSON* logs_array = cJSON_CreateArray();
    
    cJSON_AddItemToObject(root, "device_id", device_id);
    cJSON_AddItemToObject(root, "bus_id", bus_id);  // MULTI-BUS: Include bus_id in root
    cJSON_AddItemToObject(root, "logs", logs_array);

    // --- TIME REPAIR ALGORITHM ---
    time_t now_sec;
    time(&now_sec);
    uint64_t now_uptime = esp_timer_get_time();
    bool system_time_synced = (now_sec > 1704067200); // 2024 cutoff

    for (int i = 0; i < count; i++) {
        // If log is from 1970 or the 2025-12-01 placeholder AND system is now synced
        if (system_time_synced && (strncmp(logs[i].timestamp, "1970", 4) == 0 || strncmp(logs[i].timestamp, "2025-12-01", 10) == 0)) {
            // Calculate how many seconds ago this log happened
            int64_t diff_us = (int64_t)now_uptime - (int64_t)logs[i].uptime_us;
            time_t repaired_time = now_sec - (diff_us / 1000000);
            
            struct tm timeinfo;
            gmtime_r(&repaired_time, &timeinfo);
            strftime(logs[i].timestamp, sizeof(logs[i].timestamp) - 2, "%Y-%m-%dT%H:%M:%S", &timeinfo);
            strcat(logs[i].timestamp, "Z");
            
            ESP_LOGI(TAG, "♻️ Repaired timestamp for Log #%d: %s (Original was %lld seconds ago)", 
                     i, logs[i].timestamp, diff_us / 1000000);
        }

        cJSON* log_entry = cJSON_CreateObject();
        cJSON_AddStringToObject(log_entry, "timestamp", logs[i].timestamp);
        cJSON_AddNumberToObject(log_entry, "face_id", logs[i].face_id);
        
        // Add face embedding array
        cJSON* embedding_array = cJSON_CreateArray();
        for (int j = 0; j < logs[i].embedding_size && j < 128; j++) {
            cJSON_AddItemToArray(embedding_array, cJSON_CreateNumber(logs[i].face_embedding[j]));
        }
        cJSON_AddItemToObject(log_entry, "face_embedding", embedding_array);
        cJSON_AddNumberToObject(log_entry, "embedding_size", logs[i].embedding_size);
        
        // 🖼️ IMAGE DATA REMOVED - Embedding only mode
        // Note: The backend will receive NULL for image_data and use the provided face_embedding array
        cJSON_AddNullToObject(log_entry, "image_data");
        
        cJSON_AddStringToObject(log_entry, "location_type", logs[i].location_type);
        cJSON_AddNumberToObject(log_entry, "latitude", logs[i].latitude);
        cJSON_AddNumberToObject(log_entry, "longitude", logs[i].longitude);
        cJSON_AddStringToObject(log_entry, "device_id", logs[i].device_id);
        
        // Add fleet context information
        cJSON_AddStringToObject(log_entry, "bus_id", logs[i].bus_id);
        cJSON_AddStringToObject(log_entry, "route_name", logs[i].route_name);
        cJSON_AddStringToObject(log_entry, "trip_id", logs[i].trip_id);
        cJSON_AddStringToObject(log_entry, "trip_date", logs[i].trip_date);
        cJSON_AddBoolToObject(log_entry, "trip_active", logs[i].trip_active);
        
        cJSON_AddItemToArray(logs_array, log_entry);
    }
    
    char* json_string = cJSON_Print(root);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to create JSON string");
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGD(TAG, "JSON payload: %d bytes", strlen(json_string));
    
    // Configure HTTP client
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", s_config.server_url, s_config.endpoint);
    
    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,  // Reduced from 30s to 20s
        .buffer_size = 16384,  // Increased for base64 images
        .buffer_size_tx = 16384,
        .disable_auto_redirect = false,  // Enable redirects
        .max_redirection_count = 3,      // Allow up to 3 redirects
        .keep_alive_enable = false,  // Disable keep-alive to prevent stale connections
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle for HTTPS
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        if (json_string) {
            free(json_string);
        }
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }
    
    // Set headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-CAM-Logger/1.0");
    
    // Check WiFi connectivity first
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected, cannot upload logs");
        esp_http_client_cleanup(client);
        if (json_string) free(json_string);
        if (root) cJSON_Delete(root);
        update_status_error("WiFi not connected");
        return ESP_ERR_WIFI_NOT_INIT;
    }

    // Check for valid IP address
    tcpip_adapter_ip_info_t ip_info;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
    if (ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "WiFi associated but no IP address, cannot upload logs");
        esp_http_client_cleanup(client);
        if (json_string) free(json_string);
        if (root) cJSON_Delete(root);
        update_status_error("No IP address");
        return ESP_ERR_WIFI_NOT_INIT;
    }
    
    // Send request
    esp_err_t err = esp_http_client_open(client, strlen(json_string));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (json_string) {
            free(json_string);
        }
        if (root) {
            cJSON_Delete(root);
        }
        
        // Provide more specific error information
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Connection failed: %s", esp_err_to_name(err));
        update_status_error(error_msg);
        return err;
    }
    
    int written = esp_http_client_write(client, json_string, strlen(json_string));
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write HTTP data");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (json_string) {
            free(json_string);
        }
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Written %d bytes to server", written);
    
    // Get response
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    ESP_LOGI(TAG, "Server response: Status=%d, Content-Length=%d", status_code, content_length);
    
    // Read response body for logging (reduced buffer)
    if (content_length > 0 && content_length < 512) {
        char response[512];
        int read_len = esp_http_client_read(client, response, sizeof(response) - 1);
        if (read_len > 0) {
            response[read_len] = '\0';
            ESP_LOGD(TAG, "Server response: %s", response);
        }
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    // Safe cleanup with null checks
    if (json_string) {
        free(json_string);
        json_string = NULL;
    }
    if (root) {
        cJSON_Delete(root);
        root = NULL;
    }
    
    if (status_code == 200) {
        ESP_LOGI(TAG, "Successfully uploaded %d log entries", count);
        update_status_success(count);
        return ESP_OK;
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Server error: HTTP %d", status_code);
        update_status_error(error_msg);
        ESP_LOGE(TAG, "Server returned error status: %d", status_code);
        return ESP_FAIL;
    }
}

static void upload_task(void* pvParameters)
{
    ESP_LOGI(TAG, "CSV upload task started with increased stack for embeddings");
    
    face_log_entry_t logs[5]; // Smaller buffer for embedding data (5 entries max)
    int actual_count;
    
    while (s_running) {
        // Wait for trigger or timeout
        if (xSemaphoreTake(s_trigger_sem, pdMS_TO_TICKS(s_config.upload_interval_seconds * 1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Upload triggered manually");
        } else {
            ESP_LOGD(TAG, "Upload interval timeout - checking for pending logs");
        }
        
        if (!s_running) break;
        
        // Check if there are pending logs
        int pending_count = csv_logger_get_pending_count();
        if (pending_count == 0) {
            ESP_LOGD(TAG, "No pending logs to upload");
            continue;
        }
        
        ESP_LOGI(TAG, "Found %d pending logs, starting upload", pending_count);
        
        // Read pending logs
        esp_err_t err = csv_logger_read_pending_logs(logs, sizeof(logs)/sizeof(logs[0]), &actual_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read pending logs: %s", esp_err_to_name(err));
            continue;
        }
        
        if (actual_count == 0) {
            ESP_LOGW(TAG, "No logs read despite pending count > 0");
            continue;
        }
        
        // Try to upload offline buffer first if we have connectivity
        upload_offline_buffer();
        
        // Upload current logs with enhanced retry logic
        int retry_count = 0;
        bool upload_success = false;
        
        while (retry_count < s_config.max_retries && !upload_success && s_running) {
            err = upload_logs_to_server(logs, actual_count);
            if (err == ESP_OK) {
                upload_success = true;
                csv_logger_mark_uploaded(actual_count);
                ESP_LOGI(TAG, "Upload successful on attempt %d", retry_count + 1);
            } else {
                retry_count++;
                if (retry_count < s_config.max_retries) {
                    int delay_ms = calculate_backoff_delay(retry_count - 1);
                    ESP_LOGW(TAG, "Upload failed (attempt %d/%d), retrying in %d ms", 
                             retry_count, s_config.max_retries, delay_ms);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                } else {
                    ESP_LOGE(TAG, "Upload failed after %d attempts, adding to offline buffer", s_config.max_retries);
                    
                    // Add to offline buffer if upload completely failed
                    if (s_config.enable_offline_buffering) {
                        add_to_offline_buffer(logs, actual_count);
                        csv_logger_mark_uploaded(actual_count); // Remove from pending to avoid duplicate buffering
                    }
                    
                    char error_msg[128];
                    snprintf(error_msg, sizeof(error_msg), "Upload failed after %d retries", s_config.max_retries);
                    update_status_error(error_msg);
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "CSV upload task stopped");
    vTaskDelete(NULL);
}

esp_err_t csv_uploader_init(csv_uploader_config_t* config)
{
    if (!config || !config->server_url || !config->endpoint) {
        ESP_LOGE(TAG, "Invalid uploader configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&s_config, config, sizeof(csv_uploader_config_t));
    
    // Set default values for new configuration options if not set
    if (s_config.retry_backoff_base_ms == 0) {
        s_config.retry_backoff_base_ms = 1000; // 1 second base delay
    }
    if (s_config.max_retry_delay_ms == 0) {
        s_config.max_retry_delay_ms = 30000; // 30 seconds max delay
    }
    if (s_config.offline_buffer_size == 0) {
        s_config.offline_buffer_size = 50; // Default 50 entries
    }
    
    // Create semaphore for manual triggers
    s_trigger_sem = xSemaphoreCreateBinary();
    if (!s_trigger_sem) {
        ESP_LOGE(TAG, "Failed to create trigger semaphore");
        return ESP_ERR_NO_MEM;
    }
    
    // Create status mutex
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) {
        ESP_LOGE(TAG, "Failed to create status mutex");
        vSemaphoreDelete(s_trigger_sem);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize status
    memset(&s_status, 0, sizeof(s_status));
    s_status.is_online = false;
    
    // Allocate offline buffer if enabled
    if (s_config.enable_offline_buffering) {
        s_offline_buffer = malloc(s_config.offline_buffer_size * sizeof(face_log_entry_t));
        if (!s_offline_buffer) {
            ESP_LOGE(TAG, "Failed to allocate offline buffer");
            vSemaphoreDelete(s_trigger_sem);
            vSemaphoreDelete(s_status_mutex);
            return ESP_ERR_NO_MEM;
        }
        s_offline_buffer_count = 0;
        ESP_LOGI(TAG, "Offline buffer allocated: %d entries", s_config.offline_buffer_size);
        
        // Clear any old cached data on initialization
        ESP_LOGI(TAG, "Clearing offline buffer to remove old cached data");
        memset(s_offline_buffer, 0, s_config.offline_buffer_size * sizeof(face_log_entry_t));
    }
    
    ESP_LOGI(TAG, "CSV uploader initialized - URL: %s%s, Interval: %ds, Offline: %s", 
             s_config.server_url, s_config.endpoint, s_config.upload_interval_seconds,
             s_config.enable_offline_buffering ? "Enabled" : "Disabled");
    
    return ESP_OK;
}

esp_err_t csv_uploader_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Upload task already running");
        return ESP_OK;
    }
    
    s_running = true;
    
    BaseType_t result = xTaskCreate(upload_task, "csv_upload", 16384, NULL, 5, &s_upload_task);  // Increased stack for B64
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "CSV upload task started");
    return ESP_OK;
}

esp_err_t csv_uploader_trigger_now(void)
{
    if (!s_running || !s_trigger_sem) {
        ESP_LOGW(TAG, "Upload task not running or not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreGive(s_trigger_sem);
    ESP_LOGI(TAG, "Manual upload triggered");
    return ESP_OK;
}

// Removed unused: csv_uploader_stop, csv_uploader_get_status, csv_uploader_reset_status
