#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the provisioning sync task
 * 
 * This task periodically checks the central Node.js server for WiFi and URL updates.
 * If updates are found, they are saved to NVS and the device restarts.
 */
esp_err_t provisioning_sync_init(const char* node_server_url, const char* bus_id);

#ifdef __cplusplus
}
#endif
