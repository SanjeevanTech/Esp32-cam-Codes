#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Device Configuration
// ============================================================

typedef struct {
    char bus_id[32];          // Bus identifier (e.g., "BUS_JC_001")
    char route_name[64];      // Route name (e.g., "Jaffna-Colombo")
    char device_id[32];       // This device ID (e.g., "ESP32_CAM_ENTRANCE_001")
    char location_type[16];   // Camera location: "ENTRY" or "EXIT"
    char server_url[128];     // Python backend URL
} device_config_t;

// ============================================================
// Device Status (Online/Offline)
// ============================================================

typedef struct {
    bool wifi_connected;      // true = online, false = offline
    bool gps_valid;           // true = GPS fix, false = no fix
    int free_heap_bytes;      // Available RAM
    int uptime_seconds;       // Device uptime
    int wifi_rssi;            // WiFi signal strength (dBm)
    int gps_satellites;       // Number of GPS satellites
} device_status_t;

// ============================================================
// Functions
// ============================================================

esp_err_t device_config_init(device_config_t* config);
esp_err_t device_config_save(const device_config_t* config);
esp_err_t device_config_load(device_config_t* config);
esp_err_t device_get_status(device_status_t* status);

#ifdef __cplusplus
}
#endif
