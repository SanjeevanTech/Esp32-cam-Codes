/* ESPRESSIF MIT License - Optimized for AI Thinker ESP32-CAM */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include <sys/time.h>

#include "who_camera.h"
#include "who_human_face_recognition.hpp"
#include "app_wifi.h"
#include "app_httpd.hpp"
#include "app_mdns.h"
#include "driver/uart.h"
#include "gps_neo7m.hpp"
#include "gps_types.h"
#include "csv_logger.h"
#include "csv_uploader.h"
#include "device_config.h"
#include "esp_task_wdt.h"
#include "power_config_sync.h"
#include "board_heartbeat.h"

// Fixed power management integration
extern "C" {
    esp_err_t power_management_init(void);
    void power_mgmt_report_wifi_reconnect(void);
    void power_mgmt_report_camera_error(void);
    void power_mgmt_report_upload_failure(void);
    void power_mgmt_report_successful_upload(void);
    bool power_mgmt_is_trip_time(void);
    esp_err_t power_mgmt_set_normal_intervals(void);
    void enter_deep_sleep(void);  // For immediate sleep when outside trip hours
}

static const char* TAG = "APP_MAIN";

// Forward declarations for GPS functions
extern "C" {
    esp_err_t gps_init(gps_config_t *config);
    esp_err_t gps_start(void);
    gps_data_t gps_get_current_data(void);
}

// Device configuration loaded from NVS or defaults
static device_config_t g_device_config;

// GPS Configuration - Safe pins (no SD card conflict)
#define GPS_UART_PORT UART_NUM_2
#define GPS_TX_PIN 14    // ESP32 TX → NEO-7M RX
#define GPS_RX_PIN 15    // ESP32 RX ← NEO-7M TX
#define GPS_BAUD_RATE 9600
#define GPS_ENABLED true  // Enabled - no SD card conflict

static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueHttpFrame = NULL;

// NTP time synchronization - replaces hardcoded time
#include "esp_sntp.h"
static bool time_synced = false;

// Callback for SNTP sync
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "🔔 Time synchronization event received");
    time_synced = true;
}

// Check if time is synchronized by comparing with a reasonable timestamp
static bool is_time_synchronized(void)
{
    time_t now;
    time(&now);
    // If time is after 2024-01-01, consider it synchronized
    return (now > 1704067200); // Unix timestamp for 2024-01-01
}

static void initialize_system_time_with_ntp(void)
{
    // Set Sri Lanka Time Zone (UTC+5:30)
    // Format: IST is name, -5:30 is offset (negative means east of UTC in POSIX)
    setenv("TZ", "IST-5:30", 1);
    tzset();
    
    ESP_LOGI(TAG, "Starting NTP time synchronization...");
    
    if (sntp_enabled()) {
        sntp_stop();
    }
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "asia.pool.ntp.org");
    sntp_setservername(2, "time.google.com");
    sntp_setservername(3, "time.nist.gov");
    
    // Set sync mode to immediate for faster initial sync
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    // Set callback for sync event
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    sntp_init();
    
    ESP_LOGI(TAG, "NTP client started with 4 servers. Waiting for sync...");
}

// Time status monitoring task
void time_status_task(void *pvParameters)
{
    int retry = 0;
    while (!is_time_synchronized() && retry < 60) {
        if (retry % 5 == 0) {
            ESP_LOGI(TAG, "⏳ Waiting for NTP sync (try %d/60)...", retry);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    
    if (is_time_synchronized()) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "✅ NTP SYNCED SUCCESSFUL: %s", strftime_buf);
        time_synced = true;
    } else {
        ESP_LOGE(TAG, "❌ NTP sync failed after 60 seconds. Will retry in background or on WiFi reconnect.");
    }
    
    while (true) {
        // Check every 30 minutes
        vTaskDelay(pdMS_TO_TICKS(1800000));
        
        if (!is_time_synchronized()) {
            ESP_LOGW(TAG, "⚠️ Time sync lost or not achieved. Re-initializing SNTP...");
            sntp_stop();
            sntp_init();
        } else {
            ESP_LOGI(TAG, "⏰ Time check: Synchronized");
        }
    }
}

// System recovery function
static void system_recovery(const char* reason) {
    ESP_LOGE(TAG, "System recovery triggered: %s", reason);
    csv_uploader_trigger_now();
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

// System status monitoring task
void system_status_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    
    static int critical_failure_count = 0;
    
    while (true) {
        esp_task_wdt_reset();
        
        device_status_t status;
        if (device_get_status(&status) == ESP_OK) {
            // Only log critical issues
            if (status.free_heap_bytes < 40000) {
                ESP_LOGW(TAG, "Low memory: %d bytes", status.free_heap_bytes);
            }
            
            if (status.free_heap_bytes < 20000) {
                ESP_LOGE(TAG, "Critical memory: %d bytes", status.free_heap_bytes);
                critical_failure_count++;
                if (critical_failure_count > 5) {
                    system_recovery("Critical memory shortage");
                }
            } else {
                // Reset counter if memory recovered
                if (critical_failure_count > 0) critical_failure_count--;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(30000)); // Check every 30 seconds
    }
}

// Frame forwarding handled by face recognition: Camera → AI → HTTP

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Starting ESP32-CAM Face Detection System");
    
    // Device configuration initialization
    esp_err_t config_ret = device_config_load(&g_device_config);
    if (config_ret != ESP_OK) {
        ESP_LOGW(TAG, "Using default device config");
        device_config_init(&g_device_config);
    }
    
    ESP_LOGI(TAG, "Bus: %s | Device: %s | Type: %s", 
             g_device_config.bus_id, g_device_config.device_id, 
             g_device_config.location_type);
    
    // Initialize WiFi and mDNS
    app_wifi_main();
    app_mdns_main();
    
    // Initialize NTP
    initialize_system_time_with_ntp();
    
    // Wait for time synchronization (up to 30 seconds)
    // This is critical so we don't check trip hours at 1970-01-01
    ESP_LOGI(TAG, "⏰ Waiting for system time to sync (NTP/GPS)...");
    int time_wait = 0;
    while (!is_time_synchronized() && time_wait < 120) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time_wait++;
        if (time_wait % 10 == 0) {
            ESP_LOGI(TAG, "⏳ Still waiting for time sync (%ds/30s)...", time_wait/2);
        }
    }
    
    if (is_time_synchronized()) {
        ESP_LOGI(TAG, "✅ Time synchronized successfully");
    } else {
        ESP_LOGW(TAG, "⚠️ Proceeding without time sync - trip checks may be unreliable!");
    }
    
    // Wait for network to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Network diagnostics removed to save RAM
    
    // Initialize power config sync FIRST (before power management starts)
    ESP_LOGI(TAG, "🔄 Initializing power config sync...");
    esp_err_t sync_ret = power_config_sync_init(
        g_device_config.server_url,      // Server URL
        g_device_config.bus_id,           // Bus ID
        g_device_config.device_id,        // Device ID
        g_device_config.location_type     // Location (ENTRY/EXIT)
    );
    
    if (sync_ret == ESP_OK) {
        sync_ret = power_config_sync_start();
        if (sync_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Power config sync started - fetching from server...");
            
            // Wait for initial sync from server (up to 10 seconds)
            // This ensures we have the correct trip hours before starting face detection
            int sync_retry = 0;
            while (!power_config_sync_has_valid_config() && sync_retry < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
                sync_retry++;
                if (sync_retry % 4 == 0) {
                    ESP_LOGI(TAG, "⏳ Waiting for server power schedule (%d/20)...", sync_retry);
                }
            }
            
            if (power_config_sync_has_valid_config()) {
                ESP_LOGI(TAG, "✅ Power schedule synced from server successfully");
            } else {
                ESP_LOGW(TAG, "⚠️ Initial power sync timed out, using fallback defaults");
            }
        } else {
            ESP_LOGW(TAG, "⚠️ Power config sync start failed: %s", esp_err_to_name(sync_ret));
        }
    } else {
        ESP_LOGW(TAG, "⚠️ Power config sync init failed: %s", esp_err_to_name(sync_ret));
    }
    
    // Initialize board heartbeat system
    ESP_LOGI(TAG, "💓 Initializing board heartbeat...");
    esp_err_t hb_ret = board_heartbeat_init(
        g_device_config.server_url,      // Server URL
        g_device_config.bus_id,           // Bus ID
        g_device_config.device_id,        // Device ID
        g_device_config.location_type     // Location (ENTRANCE/EXIT)
    );
    
    if (hb_ret == ESP_OK) {
        hb_ret = board_heartbeat_start();
        if (hb_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Board heartbeat started - will report every 60 seconds");
        } else {
            ESP_LOGW(TAG, "⚠️ Board heartbeat start failed: %s", esp_err_to_name(hb_ret));
        }
    } else {
        ESP_LOGW(TAG, "⚠️ Board heartbeat init failed: %s", esp_err_to_name(hb_ret));
    }
    
    // Initialize power management (after sync has fetched config)
    esp_err_t power_ret = power_management_init();
    if (power_ret == ESP_OK) {
        ESP_LOGI(TAG, "Power management OK");
        
        // Set reasonable check intervals to reduce log spam
        // Trip: 60s, Idle: 300s (5min), Maintenance: 30s, Log: 300s (5min)
        esp_err_t interval_ret = power_mgmt_set_normal_intervals();
        if (interval_ret == ESP_OK) {
            ESP_LOGI(TAG, "Power management intervals configured");
        }
        
        // Multi-trip schedule auto-syncs from server every 30 seconds
        // For manual override, use power_mgmt_set_manual_override(true)
        ESP_LOGI(TAG, "✅ Using automatic schedule from server");
    } else {
        ESP_LOGE(TAG, "Power management failed: %s", esp_err_to_name(power_ret));
    }

    // Create queues (reduced size for memory optimization)
    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueHttpFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    
    if (!xQueueAIFrame || !xQueueHttpFrame) {
        ESP_LOGE(TAG, "Queue creation failed");
        return;
    }

    // Initialize GPS for face detection location tracking
    gps_config_t gps_config = {
        .uart_port = GPS_UART_PORT,
        .tx_pin = GPS_TX_PIN,
        .rx_pin = GPS_RX_PIN,
        .baud_rate = GPS_BAUD_RATE
    };
    esp_err_t gps_ret = gps_init(&gps_config);
    if (gps_ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ GPS initialized");
        gps_start();
    } else {
        ESP_LOGE(TAG, "GPS init failed: %s", esp_err_to_name(gps_ret));
    }

    // Initialize CSV logger for in-memory storage (uploads when internet available)
    csv_logger_config_t csv_config = {
        .device_id = g_device_config.device_id,
        .location_type = g_device_config.location_type,
        .bus_id = g_device_config.bus_id,
        .route_name = g_device_config.route_name,
        .csv_file_path = NULL,  // Not used - in-memory buffer
        .max_records_per_file = 10,  // In-memory buffer size
        .upload_interval_seconds = 5  // Try upload every 5 SECONDS for real-time detection
    };
    esp_err_t csv_ret = csv_logger_init(&csv_config);
    if (csv_ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ CSV logger initialized (in-memory buffer: 10 entries)");
        
        // Initialize CSV uploader (will queue uploads when internet available)
        csv_uploader_config_t uploader_config = {
            .server_url = g_device_config.server_url,
            .endpoint = "/api/face-logs",
            .upload_interval_seconds = 5,  // Try upload every 5 SECONDS for real-time detection
            .max_batch_size = 50,
            .max_retries = 5,
            .retry_backoff_base_ms = 1000,
            .max_retry_delay_ms = 60000,
            .offline_buffer_size = 500,
            .enable_offline_buffering = true
        };
        esp_err_t upload_ret = csv_uploader_init(&uploader_config);
        if (upload_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ CSV uploader initialized (auto-upload when online)");
            csv_uploader_start();
        } else {
            ESP_LOGW(TAG, "CSV uploader init failed: %s", esp_err_to_name(upload_ret));
        }
    } else {
        ESP_LOGE(TAG, "CSV logger init failed: %s", esp_err_to_name(csv_ret));
    }

    // Register camera (3 buffers for stable frame capture - matches camera fix)
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 3, xQueueAIFrame);
    ESP_LOGI(TAG, "Camera OK");
    
    // ========== CRITICAL: CHECK TRIP TIME BEFORE STARTING FACE DETECTION ==========
    // If we're outside trip hours, enter deep sleep immediately to save power
    // This prevents the bug where face detection runs outside of scheduled trips
    ESP_LOGI(TAG, "🔍 Checking if current time is within trip hours...");
    
    if (!power_mgmt_is_trip_time()) {
        ESP_LOGW(TAG, "⏰ OUTSIDE TRIP HOURS - Entering deep sleep immediately");
        ESP_LOGW(TAG, "💤 Face detection will NOT start until trip time begins");
        enter_deep_sleep();
        
        // If we return from deep sleep (shouldn't happen), restart
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "✅ WITHIN TRIP HOURS - Starting face detection");
    // ========== END TRIP TIME CHECK ==========
    
    // Log memory before face recognition
    uint32_t free_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "📊 Free heap before face recognition: %d bytes", free_before);
    
    // Face recognition ENABLED
    register_human_face_recognition(xQueueAIFrame, NULL, NULL, xQueueHttpFrame, true);
    ESP_LOGI(TAG, "✅ Face recognition ENABLED");
    
    // Log memory after
    uint32_t free_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "📊 Free heap after face recognition: %d bytes", free_after);
    
    // Register HTTP server (optimized for low memory)
    // Check if we have enough memory for HTTP server
    if (free_after > 50000) {  // Need at least 50KB free
        register_httpd(xQueueHttpFrame, NULL, true);
        ESP_LOGI(TAG, "HTTP server OK");
    } else {
        ESP_LOGW(TAG, "⚠️ Insufficient memory for HTTP server (%d bytes free)", free_after);
        ESP_LOGW(TAG, "💡 HTTP server disabled - face detection will continue without web interface");
        ESP_LOGW(TAG, "💡 To enable HTTP server, reduce face recognition features or increase PSRAM");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  🎥 FACE DETECTION SYSTEM READY");
    ESP_LOGI(TAG, "  📷 Camera: QVGA (320x240), 3 buffers");
    ESP_LOGI(TAG, "  🧠 Detection: MSR01 + MNP01 (relaxed thresholds)");
    ESP_LOGI(TAG, "  📊 Free heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

    // Start monitoring tasks
    xTaskCreate(system_status_task, "system_status", 2560, NULL, 4, NULL);
    xTaskCreate(time_status_task, "time_status", 2048, NULL, 3, NULL);
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "✅ System startup complete");
}
