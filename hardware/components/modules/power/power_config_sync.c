/*
 * Power Configuration Sync - Implementation
 * Syncs power settings from central server
 */

#include "power_config_sync.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "POWER_SYNC";

// Trip window structure for multi-trip support
typedef struct {
    int start_hour;
    int start_minute;
    int end_hour;
    int end_minute;
    char trip_name[32];
    bool active;
} trip_window_t;

// External power management functions
extern esp_err_t power_mgmt_update_schedule(int start_hour, int start_min, int end_hour, int end_min);
extern void power_mgmt_enable_sleep(void);
extern void power_mgmt_disable_sleep(void);
extern void power_mgmt_enable_maintenance_windows(int interval_minutes, int duration_minutes);
extern esp_err_t power_mgmt_set_multi_trip_windows(trip_window_t* windows, int count);

// Global to store trip windows from JSON
static trip_window_t g_trip_windows[10];
static int g_trip_window_count = 0;

// Configuration
static char g_server_url[128] = {0};
static char g_bus_id[32] = {0};
static char g_device_id[64] = {0};
static char g_location[16] = {0};
static bool g_sync_enabled = false;
static TaskHandle_t g_sync_task_handle = NULL;

// Sync interval (120 seconds = 2 minutes to reduce memory pressure)
#define SYNC_INTERVAL_MS (120 * 1000)
// Heartbeat removed - handled by board_heartbeat.c

// Current config cache
typedef struct {
    bool deep_sleep_enabled;
    int trip_start_hour;
    int trip_start_min;
    int trip_end_hour;
    int trip_end_min;
    int maintenance_interval;
    int maintenance_duration;
    bool valid;
} power_config_cache_t;

static power_config_cache_t g_cached_config = {0};

/**
 * @brief Parse time string (HH:MM) to hour and minute
 */
static bool parse_time(const char *time_str, int *hour, int *min) {
    if (!time_str || strlen(time_str) < 5) return false;
    
    char hour_str[3] = {0};
    char min_str[3] = {0};
    
    strncpy(hour_str, time_str, 2);
    strncpy(min_str, time_str + 3, 2);
    
    *hour = atoi(hour_str);
    *min = atoi(min_str);
    
    return (*hour >= 0 && *hour < 24 && *min >= 0 && *min < 60);
}

/**
 * @brief HTTP event handler
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Copy response data to buffer
            if (evt->user_data) {
                char *buffer = (char *)evt->user_data;
                int current_len = strlen(buffer);
                if (current_len + evt->data_len < 1536) {
                    memcpy(buffer + current_len, evt->data, evt->data_len);
                    buffer[current_len + evt->data_len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Fetch power configuration from server
 */
static esp_err_t fetch_power_config(power_config_cache_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    // Check WiFi connectivity and IP
    tcpip_adapter_ip_info_t ip_info;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
    if (ip_info.ip.addr == 0) {
        ESP_LOGD(TAG, "Skipping config fetch - No IP address");
        return ESP_ERR_WIFI_NOT_INIT;
    }
    
    char url[256];
    snprintf(url, sizeof(url), "%s/api/power-config?bus_id=%s", g_server_url, g_bus_id);
    
    ESP_LOGI(TAG, "Fetching config from: %s", url);
    
    // Log free heap before allocation
    ESP_LOGI(TAG, "💾 Free heap before request: %d bytes", esp_get_free_heap_size());
    
    // Allocate buffer for response (1536 bytes to save memory)
    char *response_buffer = malloc(1536);
    if (!response_buffer) {
        ESP_LOGE(TAG, "❌ Failed to allocate response buffer (need 1536 bytes, have %d free)", esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    memset(response_buffer, 0, 1536);
    
    // Log largest free block to check for fragmentation
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "💾 Largest free block: %d bytes", largest_free_block);
    
    esp_http_client_config_t http_config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 20000,  // Increased timeout for SSL handshake
        .buffer_size = 2048,  // Increased for SSL
        .buffer_size_tx = 2048,  // Increased for SSL handshake
        .user_data = response_buffer,  // Pass buffer to event handler
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle
        .is_async = false,
        .max_redirection_count = 0,  // Disable redirects to save memory
        .keep_alive_enable = false,  // Disable keep-alive to save memory
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response_buffer);
        return ESP_FAIL;
    }
    
    // Set Host header explicitly for SNI (Server Name Indication)
    // Extract hostname from URL
    char hostname[128] = {0};
    if (strncmp(url, "https://", 8) == 0) {
        const char *host_start = url + 8;
        const char *host_end = strchr(host_start, '/');
        if (host_end) {
            int host_len = host_end - host_start;
            if (host_len < sizeof(hostname)) {
                strncpy(hostname, host_start, host_len);
                hostname[host_len] = '\0';
                esp_http_client_set_header(client, "Host", hostname);
                ESP_LOGD(TAG, "Set Host header for SNI: %s", hostname);
            }
        }
    }
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d", status_code, content_length);
        
        if (status_code == 200 && strlen(response_buffer) > 0) {
                
                ESP_LOGI(TAG, "Response: %s", response_buffer);
                
                // Parse JSON response
                cJSON *root = cJSON_Parse(response_buffer);
                if (root) {
                    cJSON *deep_sleep = cJSON_GetObjectItem(root, "deep_sleep_enabled");
                    cJSON *trip_start = cJSON_GetObjectItem(root, "trip_start");
                    cJSON *trip_end = cJSON_GetObjectItem(root, "trip_end");
                    cJSON *maint_interval = cJSON_GetObjectItem(root, "maintenance_interval");
                    cJSON *maint_duration = cJSON_GetObjectItem(root, "maintenance_duration");
                    cJSON *trip_windows_array = cJSON_GetObjectItem(root, "trip_windows");
                    cJSON *server_time = cJSON_GetObjectItem(root, "current_server_time");

                    // Sync time from server ONLY if NTP hasn't worked yet
                    if (server_time && server_time->valuestring) {
                        time_t now_c;
                        time(&now_c);
                        if (now_c < 1704067200) { // Only sync if clock is still in 1970/2024
                            struct tm tm;
                            memset(&tm, 0, sizeof(struct tm));
                            if (strptime(server_time->valuestring, "%Y-%m-%d %H:%M:%S", &tm)) {
                                // Server time is UTC. mktime() uses current local TZ (IST-5:30).
                                // To get the correct UTC timestamp:
                                // Real UTC = mktime(&tm) + 5 hours 30 mins (19800 seconds)
                                time_t t = mktime(&tm) + 19800;
                                struct timeval tv = { .tv_sec = t }; 
                                settimeofday(&tv, NULL);
                                ESP_LOGI(TAG, "⏰ Backup time sync from server (UTC): %s", server_time->valuestring);
                            }
                        } else {
                            ESP_LOGD(TAG, "Skipping server time sync (NTP already accurate)");
                        }
                    }
                    
                    if (deep_sleep && trip_start && trip_end && maint_interval && maint_duration) {
                        config->deep_sleep_enabled = cJSON_IsTrue(deep_sleep);
                        
                        if (parse_time(trip_start->valuestring, &config->trip_start_hour, &config->trip_start_min) &&
                            parse_time(trip_end->valuestring, &config->trip_end_hour, &config->trip_end_min)) {
                            
                            config->maintenance_interval = maint_interval->valueint;
                            config->maintenance_duration = maint_duration->valueint;
                            config->valid = true;
                            
                            // Check if multi-trip mode is enabled (check if trip_windows array exists and has items)
                            bool has_multi_trip = trip_windows_array && cJSON_IsArray(trip_windows_array) && 
                                                 cJSON_GetArraySize(trip_windows_array) > 0;
                            
                            if (has_multi_trip) {
                                int window_count = cJSON_GetArraySize(trip_windows_array);
                                g_trip_window_count = (window_count > 10) ? 10 : window_count;
                                
                                ESP_LOGI(TAG, "✅ Config parsed successfully (Multi-Trip Mode)");
                                ESP_LOGI(TAG, "   Deep Sleep: %s", config->deep_sleep_enabled ? "enabled" : "disabled");
                                ESP_LOGI(TAG, "   Trip Windows: %d", g_trip_window_count);
                                
                                // Parse each trip window
                                for (int i = 0; i < g_trip_window_count; i++) {
                                    cJSON *window = cJSON_GetArrayItem(trip_windows_array, i);
                                    cJSON *route = cJSON_GetObjectItem(window, "route");
                                    cJSON *start = cJSON_GetObjectItem(window, "start_time");
                                    cJSON *end = cJSON_GetObjectItem(window, "end_time");
                                    cJSON *active = cJSON_GetObjectItem(window, "active");
                                    
                                    if (start && end) {
                                        // Parse start time
                                        int start_h, start_m;
                                        sscanf(start->valuestring, "%d:%d", &start_h, &start_m);
                                        
                                        // Parse end time
                                        int end_h, end_m;
                                        sscanf(end->valuestring, "%d:%d", &end_h, &end_m);
                                        
                                        // Store in global array
                                        g_trip_windows[i].start_hour = start_h;
                                        g_trip_windows[i].start_minute = start_m;
                                        g_trip_windows[i].end_hour = end_h;
                                        g_trip_windows[i].end_minute = end_m;
                                        
                                        // Use route name if available, otherwise create default name
                                        if (route && route->valuestring) {
                                            strncpy(g_trip_windows[i].trip_name, route->valuestring, 31);
                                        } else {
                                            snprintf(g_trip_windows[i].trip_name, 32, "Trip %02d:%02d-%02d:%02d", 
                                                    start_h, start_m, end_h, end_m);
                                        }
                                        g_trip_windows[i].trip_name[31] = '\0';
                                        
                                        // Set active flag (default to true if not specified)
                                        g_trip_windows[i].active = (active && cJSON_IsFalse(active)) ? false : true;
                                        
                                        ESP_LOGI(TAG, "   %d. %s: %02d:%02d - %02d:%02d %s", 
                                                i + 1, g_trip_windows[i].trip_name,
                                                start_h, start_m, end_h, end_m,
                                                g_trip_windows[i].active ? "✅" : "❌");
                                    }
                                }
                                
                                ESP_LOGI(TAG, "   Maintenance: %d min / %d min", 
                                        config->maintenance_interval, config->maintenance_duration);
                            } else {
                                g_trip_window_count = 0;  // No multi-trip windows
                                ESP_LOGI(TAG, "✅ Config parsed successfully");
                                ESP_LOGI(TAG, "   Deep Sleep: %s", config->deep_sleep_enabled ? "enabled" : "disabled");
                                ESP_LOGI(TAG, "   Trip: %02d:%02d - %02d:%02d", 
                                        config->trip_start_hour, config->trip_start_min,
                                        config->trip_end_hour, config->trip_end_min);
                                ESP_LOGI(TAG, "   Maintenance: %d min / %d min", 
                                        config->maintenance_interval, config->maintenance_duration);
                            }
                            
                            err = ESP_OK;
                        } else {
                            ESP_LOGE(TAG, "Failed to parse time strings");
                            err = ESP_FAIL;
                        }
                    } else {
                        ESP_LOGE(TAG, "Missing required fields in JSON");
                        err = ESP_FAIL;
                    }
                    
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON");
                    err = ESP_FAIL;
                }
                
        } else {
            ESP_LOGW(TAG, "HTTP request failed: status=%d or empty response", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        
        // Log detailed error information for SSL/TLS issues
        if (err == ESP_ERR_HTTP_CONNECT) {
            ESP_LOGE(TAG, "❌ Connection failed - check:");
            ESP_LOGE(TAG, "   1. Server URL is correct: %s", url);
            ESP_LOGE(TAG, "   2. Certificate bundle is enabled in sdkconfig");
            ESP_LOGE(TAG, "   3. Free heap: %d bytes", esp_get_free_heap_size());
            ESP_LOGE(TAG, "   4. Largest free block: %d bytes", 
                     heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            ESP_LOGE(TAG, "   5. WiFi is connected");
        }
    }
    
    esp_http_client_cleanup(client);
    free(response_buffer);
    return err;
}

/**
 * @brief Apply power configuration
 */
static esp_err_t apply_power_config(const power_config_cache_t *config) {
    if (!config || !config->valid) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "🔄 Applying power configuration...");
    
    // Check if we have multi-trip windows from server
    esp_err_t err;
    if (g_trip_window_count > 0) {
        // Use multi-trip windows from server
        ESP_LOGI(TAG, "📅 Applying %d trip windows from server", g_trip_window_count);
        err = power_mgmt_set_multi_trip_windows(g_trip_windows, g_trip_window_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set multi-trip windows: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "✅ Multi-trip schedule applied successfully");
    } else {
        // Fallback: Convert single trip to 1-window multi-trip
        trip_window_t trip = {
            .start_hour = config->trip_start_hour,
            .start_minute = config->trip_start_min,
            .end_hour = config->trip_end_hour,
            .end_minute = config->trip_end_min,
            .trip_name = "Server Trip",
            .active = true
        };
        
        err = power_mgmt_set_multi_trip_windows(&trip, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set trip window: %s", esp_err_to_name(err));
            return err;
        }
    }
    
    // ALWAYS update legacy schedule so maintenance windows have correct reference
    power_mgmt_update_schedule(
        config->trip_start_hour, config->trip_start_min,
        config->trip_end_hour, config->trip_end_min
    );
    
    // Enable/disable deep sleep
    if (config->deep_sleep_enabled) {
        power_mgmt_enable_sleep();
        ESP_LOGI(TAG, "✅ Deep sleep enabled");
    } else {
        power_mgmt_disable_sleep();
        ESP_LOGI(TAG, "✅ Deep sleep disabled");
    }
    
    // Update maintenance windows
    power_mgmt_enable_maintenance_windows(
        config->maintenance_interval,
        config->maintenance_duration
    );
    
    ESP_LOGI(TAG, "✅ Power configuration applied successfully");
    
    return ESP_OK;
}

/**
 * @brief Check if configuration has changed
 */
static bool config_has_changed(const power_config_cache_t *new_config) {
    if (!g_cached_config.valid || !new_config->valid) return true;
    
    return (g_cached_config.deep_sleep_enabled != new_config->deep_sleep_enabled ||
            g_cached_config.trip_start_hour != new_config->trip_start_hour ||
            g_cached_config.trip_start_min != new_config->trip_start_min ||
            g_cached_config.trip_end_hour != new_config->trip_end_hour ||
            g_cached_config.trip_end_min != new_config->trip_end_min ||
            g_cached_config.maintenance_interval != new_config->maintenance_interval ||
            g_cached_config.maintenance_duration != new_config->maintenance_duration);
}

// Heartbeat function removed - handled by board_heartbeat.c

/**
 * @brief Check if we have a valid config from server
 */
bool power_config_sync_has_valid_config(void) {
    return g_cached_config.valid;
}

/**
 * @brief Power config sync task
 */
static void power_config_sync_task(void *pvParameters) {
    ESP_LOGI(TAG, "🚀 Power config sync task started");
    ESP_LOGI(TAG, "   Server: %s", g_server_url);
    ESP_LOGI(TAG, "   Bus ID: %s", g_bus_id);
    ESP_LOGI(TAG, "   Device: %s (%s)", g_device_id, g_location);
    
    // Initialize so the first sync happens immediately
    TickType_t last_sync = xTaskGetTickCount() - pdMS_TO_TICKS(SYNC_INTERVAL_MS) - 1;
    
    // First sync happens immediately
    // This allows the system to know trip status at boot
    
    while (g_sync_enabled) {
        TickType_t now = xTaskGetTickCount();
        
        // Check if it's time to sync
        // Adaptive interval: 5 seconds if no config yet, SYNC_INTERVAL_MS otherwise
        uint32_t current_interval = g_cached_config.valid ? SYNC_INTERVAL_MS : 5000;
        
        if ((now - last_sync) >= pdMS_TO_TICKS(current_interval)) {
            power_config_cache_t new_config = {0};
            
            if (fetch_power_config(&new_config) == ESP_OK) {
                if (config_has_changed(&new_config)) {
                    ESP_LOGI(TAG, "🔄 Configuration changed, applying...");
                    
                    if (apply_power_config(&new_config) == ESP_OK) {
                        // Update cache
                        memcpy(&g_cached_config, &new_config, sizeof(power_config_cache_t));
                        ESP_LOGI(TAG, "✅ Configuration updated successfully");
                    } else {
                        ESP_LOGE(TAG, "❌ Failed to apply configuration");
                    }
                } else {
                    ESP_LOGD(TAG, "ℹ️ Configuration unchanged");
                }
            } else {
                // Only log warning if we have a valid config (maintenance mode)
                // If starting up, keep it slightly quieter as we retry frequently
                if (g_cached_config.valid) {
                    ESP_LOGW(TAG, "⚠️ Failed to fetch configuration (server offline?)");
                } else {
                     ESP_LOGD(TAG, "Waiting for server/network...");
                }
            }
            
            last_sync = now;
        }
        
        // Heartbeat handled by board_heartbeat.c (every 60 seconds)
        
        // Sleep for 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "Power config sync task stopped");
    g_sync_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Initialize power config sync
 */
esp_err_t power_config_sync_init(const char *server_url, const char *bus_id, 
                                  const char *device_id, const char *location) {
    if (!server_url || !bus_id || !device_id || !location) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);
    strncpy(g_bus_id, bus_id, sizeof(g_bus_id) - 1);
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    strncpy(g_location, location, sizeof(g_location) - 1);
    
    g_sync_enabled = true;
    
    ESP_LOGI(TAG, "✅ Power config sync initialized");
    
    return ESP_OK;
}

/**
 * @brief Start power config sync task
 */
esp_err_t power_config_sync_start(void) {
    if (!g_sync_enabled) {
        ESP_LOGE(TAG, "Sync not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_sync_task_handle != NULL) {
        ESP_LOGW(TAG, "Sync task already running");
        return ESP_OK;
    }
    
    // Increased stack to 8192 for HTTPS + JSON parsing (SSL needs more stack)
    // HTTPS client + mbedtls + cJSON parsing requires significant stack space
    // mbedtls_ssl_setup needs large stack for SSL context allocation
    BaseType_t ret = xTaskCreate(power_config_sync_task, "power_sync", 8192, NULL, 5, &g_sync_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create sync task (insufficient memory?)");
        ESP_LOGI(TAG, "💡 Try disabling other features to free up RAM");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Power config sync task started");
    
    return ESP_OK;
}

// Removed unused: power_config_sync_stop, power_config_sync_now, power_config_sync_is_enabled
