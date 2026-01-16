/**
 * @file board_heartbeat.h
 * @brief ESP32 Board Heartbeat System
 * 
 * Sends periodic heartbeat signals to Python server to:
 * - Update board online/offline status
 * - Report location and IP address
 */

#ifndef BOARD_HEARTBEAT_H
#define BOARD_HEARTBEAT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize heartbeat system
 * 
 * @param server_url Python server URL (e.g., "http://10.180.130.50:8888")
 * @param bus_id Bus identifier (e.g., "BUS_JC_001")
 * @param device_id Device identifier (e.g., "ESP32_CAM_ENTRY_001")
 * @param location Board location (e.g., "ENTRY" or "EXIT")
 * @return ESP_OK on success
 */
esp_err_t board_heartbeat_init(const char* server_url, const char* bus_id, 
                               const char* device_id, const char* location);

/**
 * @brief Start heartbeat task (sends heartbeat every 60 seconds)
 * @return ESP_OK on success
 */
esp_err_t board_heartbeat_start(void);

#ifdef __cplusplus
}
#endif

#endif // BOARD_HEARTBEAT_H
