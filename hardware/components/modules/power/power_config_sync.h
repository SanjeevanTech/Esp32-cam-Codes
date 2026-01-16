/*
 * Power Configuration Sync - Header
 * Syncs power settings from central server
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize power config sync
 * 
 * @param server_url Server URL (e.g., "http://10.180.130.50:8888")
 * @param bus_id Bus ID (e.g., "BUS_JC_001")
 * @param device_id Device ID (e.g., "ESP32_CAM_ENTRANCE_001")
 * @param location Location type (e.g., "ENTRY" or "EXIT")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t power_config_sync_init(const char *server_url, const char *bus_id, 
                                  const char *device_id, const char *location);

/**
 * @brief Start power config sync task
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t power_config_sync_start(void);
bool power_config_sync_has_valid_config(void);

#ifdef __cplusplus
}
#endif
