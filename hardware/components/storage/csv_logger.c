#include "csv_logger.h"
#include "csv_uploader.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char* TAG = "CSV_LOGGER";

static csv_logger_config_t s_config;
static SemaphoreHandle_t s_csv_mutex = NULL;
static bool s_initialized = false;

// In-memory buffer for face detection logs
#define MAX_LOG_ENTRIES 5
static face_log_entry_t s_log_buffer[MAX_LOG_ENTRIES];
static int s_log_count = 0;

static void get_current_timestamp(char* timestamp, size_t size)
{
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    
    // Check if time is synchronized (after 2024-01-01)
    if (now < 1704067200) {
        // Time not synced, use a placeholder that backend will replace
        snprintf(timestamp, size, "1970-01-01T00:00:00Z");
        ESP_LOGW(TAG, "⚠️ Time not synced yet, using placeholder timestamp");
        return;
    }
    
    // Use gmtime_r for UTC time (not localtime_r for local time)
    gmtime_r(&now, &timeinfo);
    // Format as ISO 8601 with Z suffix for UTC
    strftime(timestamp, size - 1, "%Y-%m-%dT%H:%M:%S", &timeinfo);
    strcat(timestamp, "Z");  // Append Z to indicate UTC timezone
}

esp_err_t csv_logger_init(csv_logger_config_t* config)
{
    if (s_initialized) return ESP_OK;
    if (!config || !config->device_id || !config->location_type) return ESP_ERR_INVALID_ARG;
    
    memcpy(&s_config, config, sizeof(csv_logger_config_t));
    
    s_csv_mutex = xSemaphoreCreateMutex();
    if (!s_csv_mutex) return ESP_ERR_NO_MEM;
    
    s_log_count = 0;
    memset(s_log_buffer, 0, sizeof(s_log_buffer));
    
    s_initialized = true;
    
    ESP_LOGI(TAG, "CSV logger initialized - Device: %s, Type: %s, In-memory buffer: %d entries", 
             s_config.device_id, s_config.location_type, MAX_LOG_ENTRIES);
    
    return ESP_OK;
}

esp_err_t csv_logger_log_face(int face_id, float* face_embedding, int embedding_size, csv_gps_data_t gps_data, uint8_t* image_buf, size_t image_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    face_log_entry_t entry;
    get_current_timestamp(entry.timestamp, sizeof(entry.timestamp));
    
    entry.face_id = face_id;
    
    // Copy face embedding
    if (face_embedding && embedding_size > 0 && embedding_size <= 128) {
        memcpy(entry.face_embedding, face_embedding, embedding_size * sizeof(float));
        entry.embedding_size = embedding_size;
    } else {
        memset(entry.face_embedding, 0, sizeof(entry.face_embedding));
        entry.embedding_size = 0;
    }
    
    strncpy(entry.location_type, s_config.location_type, sizeof(entry.location_type) - 1);
    entry.location_type[sizeof(entry.location_type) - 1] = '\0';
    entry.latitude = gps_data.valid ? gps_data.latitude : 0.0;
    entry.longitude = gps_data.valid ? gps_data.longitude : 0.0;
    entry.altitude = gps_data.valid ? gps_data.altitude : 0.0;
    entry.satellites = gps_data.valid ? gps_data.satellites : 0;
    strncpy(entry.device_id, s_config.device_id, sizeof(entry.device_id) - 1);
    entry.device_id[sizeof(entry.device_id) - 1] = '\0';
    
    // Fleet context from configuration
    strncpy(entry.bus_id, s_config.bus_id ? s_config.bus_id : "UNKNOWN", sizeof(entry.bus_id) - 1);
    entry.bus_id[sizeof(entry.bus_id) - 1] = '\0';
    
    strncpy(entry.route_name, s_config.route_name ? s_config.route_name : "UNKNOWN", sizeof(entry.route_name) - 1);
    entry.route_name[sizeof(entry.route_name) - 1] = '\0';
    
    // Trip context handled by backend - send empty values
    strcpy(entry.trip_id, "");
    strcpy(entry.trip_date, "");
    entry.trip_active = false;
    entry.uptime_us = esp_timer_get_time(); // Record precise uptime for time repair logic
    
    // Store image data if provided (allocate memory)
    if (image_buf && image_len > 0) {
        uint8_t* stored_img = (uint8_t*)malloc(image_len);
        if (stored_img) {
            memcpy(stored_img, image_buf, image_len);
            entry.image_ptr = (uintptr_t)stored_img;
            entry.image_len = image_len;
            ESP_LOGI(TAG, "Stored image data in log entry (%d bytes)", image_len);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for image data (%d bytes)", image_len);
            entry.image_ptr = 0;
            entry.image_len = 0;
        }
    } else {
        entry.image_ptr = 0;
        entry.image_len = 0;
    }
    
    // Store in memory buffer (circular buffer)
    if (s_log_count >= MAX_LOG_ENTRIES) {
        // Free the oldest image buffer before shifting
        if (s_log_buffer[0].image_ptr) {
            free((void*)s_log_buffer[0].image_ptr);
        }
        
        for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) {
            s_log_buffer[i] = s_log_buffer[i + 1];
        }
        s_log_count = MAX_LOG_ENTRIES - 1;
    }
    
    s_log_buffer[s_log_count] = entry;
    s_log_count++;
    
    ESP_LOGI(TAG, "Logged face: ID=%d, Embedding=%d, GPS=%.6f,%.6f, Bus=%s, Trip=%s, Buffer=%d/%d",
             face_id, entry.embedding_size, entry.latitude, entry.longitude,
             entry.bus_id, entry.trip_id, s_log_count, MAX_LOG_ENTRIES);
    
    xSemaphoreGive(s_csv_mutex);
    
    // Trigger background upload
    csv_uploader_trigger_now();
    
    return ESP_OK;
}

int csv_logger_get_pending_count(void)
{
    return s_log_count;
}

esp_err_t csv_logger_read_pending_logs(face_log_entry_t* logs, int max_count, int* actual_count)
{
    if (!logs || !actual_count) return ESP_ERR_INVALID_ARG;
    
    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    int count = (s_log_count < max_count) ? s_log_count : max_count;
    for (int i = 0; i < count; i++) {
        logs[i] = s_log_buffer[i];
    }
    
    *actual_count = count;
    xSemaphoreGive(s_csv_mutex);
    return ESP_OK;
}

esp_err_t csv_logger_mark_uploaded(int count)
{
    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (count >= s_log_count) {
        // Free all image buffers being removed
        for (int i = 0; i < s_log_count; i++) {
            if (s_log_buffer[i].image_ptr) {
                free((void*)s_log_buffer[i].image_ptr);
                s_log_buffer[i].image_ptr = 0;
            }
        }
        s_log_count = 0;
    } else {
        // Free image buffers for entries being removed
        for (int i = 0; i < count; i++) {
            if (s_log_buffer[i].image_ptr) {
                free((void*)s_log_buffer[i].image_ptr);
                s_log_buffer[i].image_ptr = 0;
            }
        }
        
        for (int i = 0; i < s_log_count - count; i++) {
            s_log_buffer[i] = s_log_buffer[i + count];
        }
        s_log_count -= count;
    }
    
    ESP_LOGI(TAG, "Marked %d entries as uploaded, %d remaining", count, s_log_count);
    
    xSemaphoreGive(s_csv_mutex);
    return ESP_OK;
}

// csv_logger_trigger_upload removed - unused (csv_uploader_trigger_now is used instead)
