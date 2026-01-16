#pragma once

#include "esp_err.h"
#include "csv_logger.h"

#ifdef __cplusplus
extern "C" {
#endif

// CSV uploader configuration
typedef struct {
    const char* server_url;
    const char* endpoint;  // e.g., "/api/face-logs"
    int upload_interval_seconds;
    int max_batch_size;
    int max_retries;
    int retry_backoff_base_ms;      // Base delay for exponential backoff
    int max_retry_delay_ms;         // Maximum retry delay
    int offline_buffer_size;        // Maximum entries to buffer offline
    bool enable_offline_buffering;  // Enable local buffering during network issues
} csv_uploader_config_t;

/**
 * Initialize CSV uploader
 */
esp_err_t csv_uploader_init(csv_uploader_config_t* config);

/**
 * Start upload task
 */
esp_err_t csv_uploader_start(void);

/**
 * Trigger immediate upload
 */
esp_err_t csv_uploader_trigger_now(void);

// Removed unused: csv_uploader_stop, csv_uploader_get_status, csv_uploader_reset_status
// Status struct kept for internal use only
typedef struct {
    bool is_online;
    int pending_uploads;
    int failed_uploads;
    int successful_uploads;
    int offline_buffer_count;
    char last_error[128];
    int64_t last_successful_upload_time;
    int consecutive_failures;
} csv_uploader_status_t;

#ifdef __cplusplus
}
#endif