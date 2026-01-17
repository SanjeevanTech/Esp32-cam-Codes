#include "device_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char* TAG = "DEVICE_CFG";
static const char* NVS_NAMESPACE = "device_config";

// Global configuration
static device_config_t g_device_config;

// External GPS function
#include "gps_types.h"
extern gps_data_t gps_get_current_data(void);

// ============================================================
// ⚠️ CHANGE THESE FOR ENTRY vs EXIT CAMERA:
// ============================================================
//   ENTRY CAMERA:
//     device_id = "ESP32_CAM_ENTRANCE_001"
//     location_type = "ENTRY"
//
//   EXIT CAMERA:
//     device_id = "ESP32_CAM_EXIT_001"
//     location_type = "EXIT"
// ============================================================
static void set_default_config(device_config_t* config) {
    strcpy(config->bus_id, "BUS_JC_001");
    strcpy(config->route_name, "AUTO_DETECT");
    strcpy(config->device_id, "ESP32_CAM_ENTRANCE_001");
    strcpy(config->location_type, "ENTRY");
    strcpy(config->server_url, "http://52.66.122.5:8888");
    strcpy(config->wifi_ssid, "Sanjeevan");
    strcpy(config->wifi_password, "12345678");
}

esp_err_t device_config_init(device_config_t* config) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Use default configuration
    set_default_config(config);
    device_config_save(config);
    
    // Copy to global
    memcpy(&g_device_config, config, sizeof(device_config_t));
    
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "Device Configuration:");
    ESP_LOGI(TAG, "  Bus: %s", config->bus_id);
    ESP_LOGI(TAG, "  Device: %s", config->device_id);
    ESP_LOGI(TAG, "  Type: %s", config->location_type);
    ESP_LOGI(TAG, "  Server: %s", config->server_url);
    ESP_LOGI(TAG, "  WiFi SSID: %s", config->wifi_ssid);
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    
    return ESP_OK;
}

esp_err_t device_config_save(const device_config_t* config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_blob(nvs_handle, "config", config, sizeof(device_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save error: %s", esp_err_to_name(err));
    }
    
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err;
}

esp_err_t device_config_load(device_config_t* config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t size = sizeof(device_config_t);
    err = nvs_get_blob(nvs_handle, "config", config, &size);
    nvs_close(nvs_handle);
    
    return err;
}

esp_err_t device_get_status(device_status_t* status) {
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(status, 0, sizeof(device_status_t));
    
    // System info
    status->free_heap_bytes = esp_get_free_heap_size();
    status->uptime_seconds = esp_timer_get_time() / 1000000;
    
    // WiFi status (online/offline indicator)
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        status->wifi_connected = true;
        status->wifi_rssi = ap_info.rssi;
    } else {
        status->wifi_connected = false;
        status->wifi_rssi = 0;
    }
    
    // GPS status
    gps_data_t gps = gps_get_current_data();
    status->gps_valid = gps.valid;
    status->gps_satellites = gps.satellites;
    
    return ESP_OK;
}
