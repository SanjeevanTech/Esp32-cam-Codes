/* ESPRESSIF MIT License
 * 
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 * 
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "mdns.h"
#include "app_wifi.h"

/* 
 * ========================================
 * 🔧 EASY WIFI CONFIGURATION - EDIT HERE
 * ========================================
 */

// No longer using hardcoded macros here - passed via app_wifi_main
#define WIFI_MAXIMUM_RETRY  5          // Max connection retries

static const char *TAG = "camera wifi";
static int s_retry_num = 0;
static char current_ssid[32] = {0};
static char current_password[64] = {0};

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "✅ WiFi is Connected - Got IP: %s",
                 ip4addr_ntoa((const ip4_addr_t*)&event->event_info.got_ip.ip_info.ip));
        s_retry_num = 0;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    {
        if (s_retry_num < WIFI_MAXIMUM_RETRY)
        {
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi disconnected, retry %d/%d...", s_retry_num, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGW(TAG, "WiFi unstable (5 failures). Entering OFFLINE MODE.");
            ESP_LOGW(TAG, "Logs will be buffered and sent once WiFi is stable again.");
        }
        break;
    }
    default:
        break;
    }
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}

void wifi_init_sta(const char* ssid, const char* password)
{
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    snprintf((char *)wifi_config.sta.ssid, 32, "%s", ssid);
    snprintf((char *)wifi_config.sta.password, 64, "%s", password);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    ESP_LOGI(TAG, "📡 Connecting to WiFi - SSID: %s", ssid);
}

void wifi_recovery_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds (aggressively reconnect)
        
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
            ESP_LOGI(TAG, "📡 Periodic WiFi check: Still offline. Resetting retries and attempting reconnection...");
            s_retry_num = 0;
            esp_wifi_connect();
        }
    }
}

void app_wifi_main(const char* ssid, const char* password)
{
    if (ssid) strncpy(current_ssid, ssid, 31);
    if (password) strncpy(current_password, password, 63);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_mode_t mode = WIFI_MODE_STA;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    wifi_init_sta(current_ssid, current_password);
    
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // Start the recovery task to handle "Stable WiFi" detection
    xTaskCreate(wifi_recovery_task, "wifi_recovery", 2048, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "✅ WiFi initialization completed (Offline Mode supported)");
}
