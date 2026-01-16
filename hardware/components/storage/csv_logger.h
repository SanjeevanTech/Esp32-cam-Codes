#pragma once

#include "esp_err.h"
#include <stdbool.h>

// GPS data structure - compatible with gps_types.h
typedef struct {
    float latitude;
    float longitude;
    float altitude;
    int satellites;
    bool valid;
    char timestamp[32];
} csv_gps_data_t;

#ifdef __cplusplus
extern "C" {
#endif

// CSV logger configuration
typedef struct {
    const char* device_id;
    const char* location_type;  // "ENTRY" or "EXIT"
    const char* bus_id;
    const char* route_name;
    const char* csv_file_path;
    int max_records_per_file;
    int upload_interval_seconds;
} csv_logger_config_t;

// Face detection log entry with embedding and fleet context
typedef struct {
    char timestamp[32];
    int face_id;                    // Keep for backward compatibility
    float face_embedding[128];      // Face embedding vector (128 dimensions)
    int embedding_size;             // Size of embedding (usually 128)
    char location_type[16];
    double latitude;
    double longitude;
    double altitude;
    int satellites;
    char device_id[32];
    // Fleet context fields
    char bus_id[32];
    char route_name[64];
    char trip_id[64];
    char trip_date[16];
    bool trip_active;
    uintptr_t image_ptr;            // NEW: Pointer to JPG buffer (cast to uint8_t*)
    size_t image_len;               // NEW: Length of JPG buffer
    uint64_t uptime_us;             // NEW: Uptime in microseconds
} face_log_entry_t;

/**
 * Initialize CSV logger system
 */
esp_err_t csv_logger_init(csv_logger_config_t* config);

/**
 * Log a face detection event with embedding and optional image
 */
esp_err_t csv_logger_log_face(int face_id, float* face_embedding, int embedding_size, csv_gps_data_t gps_data, uint8_t* image_buf, size_t image_len);

/**
 * Get pending log count
 */
int csv_logger_get_pending_count(void);

/**
 * Read pending logs for upload
 */
esp_err_t csv_logger_read_pending_logs(face_log_entry_t* logs, int max_count, int* actual_count);

/**
 * Mark logs as uploaded and remove from pending
 */
esp_err_t csv_logger_mark_uploaded(int count);

// Removed unimplemented functions:
// - csv_logger_trigger_upload (use csv_uploader_trigger_now instead)
// - csv_logger_get_csv_files (SD card not used)
// - csv_logger_read_csv_file (SD card not used)
// - csv_logger_delete_csv_file (SD card not used)
// - csv_logger_check_sd_card (SD card not used)
// - csv_logger_remount_sd_card (SD card not used)
// - csv_logger_save_server_response_as_csv (not needed)

#ifdef __cplusplus
}
#endif