/*
ESP32 Power Management - FIXED VERSION
Handles automatic on/off without breaking web server
*/

#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <time.h>
#include <sys/time.h>

// ESP32-CAM LED pin (white flash LED)
#define LED_GPIO GPIO_NUM_4

static const char* TAG = "POWER_MGMT";

// Trip window structure for multi-trip support
#define MAX_TRIP_WINDOWS 10

typedef struct {
    int start_hour;
    int start_minute;
    int end_hour;
    int end_minute;
    char trip_name[32];
    bool active;
} trip_window_t;

// Power management configuration
typedef struct {
    // Legacy single trip (for backward compatibility)
    int trip_start_hour;      // 6 (6:00 AM)
    int trip_start_minute;    // 0
    int trip_end_hour;        // 17 (5:00 PM)
    int trip_end_minute;      // 30
    
    // Multi-trip support
    bool use_multi_trip;      // Use multiple trip windows instead of single
    int trip_window_count;
    trip_window_t trip_windows[MAX_TRIP_WINDOWS];
    bool manual_override;     // If true, ignore server updates
    
    bool enable_deep_sleep;   // Enable deep sleep during off hours
    int health_check_interval_sec; // Health check every 5 minutes
    
    // Check intervals (configurable)
    int trip_check_interval_sec;      // How often to check during trip time (default: 60s)
    int idle_check_interval_sec;      // How often to check when idle (default: 300s = 5min)
    int maintenance_check_interval_sec; // How often to check during maintenance (default: 30s)
    int log_interval_sec;             // How often to log status (default: 300s = 5min)
    
    // Maintenance window settings
    bool enable_maintenance_windows;
    int maintenance_interval_minutes;
    int maintenance_duration_minutes;
} power_config_t;

static power_config_t g_power_config = {
    // Legacy single trip - DEFAULT OFF (10:00-10:00) until synced from server
    .trip_start_hour = 10,
    .trip_start_minute = 0,
    .trip_end_hour = 10,
    .trip_end_minute = 0,
    
    // Multi-trip (disabled by default)
    .use_multi_trip = false,
    .trip_window_count = 0,
    .manual_override = false,
    
    .enable_deep_sleep = true,   // DEFAULT: ENABLED - deep sleep when outside trip times
    .health_check_interval_sec = 150,  // 5 minutes
    
    // Check intervals - much more reasonable defaults
    .trip_check_interval_sec = 60,      // Check every 1 minute during trip
    .idle_check_interval_sec = 300,     // Check every 5 minutes when idle
    .maintenance_check_interval_sec = 30, // Check every 30 seconds during maintenance
    .log_interval_sec = 300,            // Log status every 5 minutes
    
    // Maintenance windows - ENABLED BY DEFAULT
    .enable_maintenance_windows = true,  // ENABLED by default
    .maintenance_interval_minutes = 5,   // Wake every 5 minutes
    .maintenance_duration_minutes = 3    // Stay awake for 3 minutes
};

// System health monitoring
typedef struct {
    uint32_t free_heap;
    int wifi_reconnects;
    int camera_errors;
    int upload_failures;
    time_t last_successful_upload;
    bool system_healthy;
} system_health_t;

static system_health_t g_system_health = {0};

// Task handle for power management
static TaskHandle_t g_power_task_handle = NULL;

static bool is_trip_time(void)
{
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    int current_hour = timeinfo.tm_hour;
    int current_minute = timeinfo.tm_min;
    int current_time_minutes = current_hour * 60 + current_minute;
    
    bool is_trip = false;
    static time_t last_log_time = 0;
    time_t current_time = time(NULL);
    
    // Multi-trip mode
    if (g_power_config.use_multi_trip && g_power_config.trip_window_count > 0) {
        // Check if current time is within ANY trip window
        for (int i = 0; i < g_power_config.trip_window_count; i++) {
            // Skip inactive trips
            if (!g_power_config.trip_windows[i].active) {
                continue;
            }
            
            int trip_start_minutes = g_power_config.trip_windows[i].start_hour * 60 + 
                                    g_power_config.trip_windows[i].start_minute;
            int trip_end_minutes = g_power_config.trip_windows[i].end_hour * 60 + 
                                  g_power_config.trip_windows[i].end_minute;
            
            // Debug: Log the comparison
            ESP_LOGD(TAG, "Checking trip %d: %s (%d-%d vs current %d)", 
                    i, g_power_config.trip_windows[i].trip_name,
                    trip_start_minutes, trip_end_minutes, current_time_minutes);
            
            if (trip_start_minutes <= trip_end_minutes) {
                // Same day trip
                if (current_time_minutes >= trip_start_minutes && 
                    current_time_minutes <= trip_end_minutes) {
                    is_trip = true;
                    
                    if (current_time - last_log_time >= g_power_config.log_interval_sec) {
                        ESP_LOGD(TAG, "🕐 Multi-Trip: Current=%02d:%02d, InTrip=%s (%s - %02d:%02d to %02d:%02d)", 
                                current_hour, current_minute, "YES",
                                g_power_config.trip_windows[i].trip_name,
                                g_power_config.trip_windows[i].start_hour,
                                g_power_config.trip_windows[i].start_minute,
                                g_power_config.trip_windows[i].end_hour,
                                g_power_config.trip_windows[i].end_minute);
                        last_log_time = current_time;
                    }
                    break;
                }
            } else {
                // Overnight trip
                if (current_time_minutes >= trip_start_minutes || 
                    current_time_minutes <= trip_end_minutes) {
                    is_trip = true;
                    
                    if (current_time - last_log_time >= g_power_config.log_interval_sec) {
                        ESP_LOGD(TAG, "🕐 Multi-Trip: Current=%02d:%02d, InTrip=%s (%s - %02d:%02d to %02d:%02d)", 
                                current_hour, current_minute, "YES",
                                g_power_config.trip_windows[i].trip_name,
                                g_power_config.trip_windows[i].start_hour,
                                g_power_config.trip_windows[i].start_minute,
                                g_power_config.trip_windows[i].end_hour,
                                g_power_config.trip_windows[i].end_minute);
                        last_log_time = current_time;
                    }
                    break;
                }
            }
        }
        
        if (!is_trip && current_time - last_log_time >= g_power_config.log_interval_sec) {
            ESP_LOGD(TAG, "🕐 Multi-Trip: Current=%02d:%02d, InTrip=NO (%d windows)", 
                    current_hour, current_minute, g_power_config.trip_window_count);
            last_log_time = current_time;
        }
        
        return is_trip;
    }
    
    // Legacy single trip mode
    int trip_start_minutes = g_power_config.trip_start_hour * 60 + g_power_config.trip_start_minute;
    int trip_end_minutes = g_power_config.trip_end_hour * 60 + g_power_config.trip_end_minute;
    
    // Check if current time is within trip hours
    if (trip_start_minutes <= trip_end_minutes) {
        // Same day trip (e.g., 6:00 AM to 5:30 PM)
        is_trip = (current_time_minutes >= trip_start_minutes && 
                   current_time_minutes <= trip_end_minutes);
    } else {
        // Overnight trip (e.g., 6:00 PM to 6:00 AM next day)
        is_trip = (current_time_minutes >= trip_start_minutes || 
                   current_time_minutes <= trip_end_minutes);
    }
    
    // Controlled logging based on configurable interval
    if (current_time - last_log_time >= g_power_config.log_interval_sec) {
        ESP_LOGD(TAG, "🕐 Time Check: Current=%02d:%02d, Trip=%02d:%02d-%02d:%02d, InTrip=%s", 
                 current_hour, current_minute,
                 g_power_config.trip_start_hour, g_power_config.trip_start_minute,
                 g_power_config.trip_end_hour, g_power_config.trip_end_minute,
                 is_trip ? "YES" : "NO");
        last_log_time = current_time;
    }
    
    return is_trip;
}

static bool is_maintenance_window(void)
{
    if (!g_power_config.enable_maintenance_windows) {
        return false;
    }
    
    // Safety check: if interval is 0, maintenance is disabled
    if (g_power_config.maintenance_interval_minutes == 0) {
        return false;
    }
    
    // Don't activate maintenance during trip time
    if (is_trip_time()) {
        return false;
    }
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int trip_end_minutes = g_power_config.trip_end_hour * 60 + g_power_config.trip_end_minute;
    
    // Calculate minutes since trip ended
    int minutes_since_trip_end;
    if (current_minutes >= trip_end_minutes) {
        minutes_since_trip_end = current_minutes - trip_end_minutes;
    } else {
        minutes_since_trip_end = (24 * 60) - trip_end_minutes + current_minutes;
    }
    
    // Check if we're in a maintenance window
    int window_position = minutes_since_trip_end % g_power_config.maintenance_interval_minutes;
    return (window_position < g_power_config.maintenance_duration_minutes);
}

void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "🔴 Preparing for deep sleep...");
    
    // Turn off LED to save battery - force it multiple times to ensure it's off
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LED_GPIO, 0);
    
    // Hold LED pin LOW during deep sleep
    gpio_hold_en(LED_GPIO);
    gpio_deep_sleep_hold_en();
    
    ESP_LOGI(TAG, "💡 LED turned OFF and held LOW during sleep");
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    uint64_t sleep_duration_sec;
    
    // Safety check: if maintenance is enabled but interval is 0, disable it
    if (g_power_config.enable_maintenance_windows && g_power_config.maintenance_interval_minutes == 0) {
        ESP_LOGW(TAG, "⚠️ Maintenance enabled but interval=0, disabling maintenance");
        g_power_config.enable_maintenance_windows = false;
    }
    
    if (g_power_config.enable_maintenance_windows && g_power_config.maintenance_interval_minutes > 0) {
        // Sleep until next maintenance window
        int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        int trip_end_minutes = g_power_config.trip_end_hour * 60 + g_power_config.trip_end_minute;
        
        int minutes_since_trip_end;
        if (current_minutes >= trip_end_minutes) {
            minutes_since_trip_end = current_minutes - trip_end_minutes;
        } else {
            minutes_since_trip_end = (24 * 60) - trip_end_minutes + current_minutes;
        }
        
        // Calculate minutes until next maintenance window
        int window_position = minutes_since_trip_end % g_power_config.maintenance_interval_minutes;
        int minutes_until_next_window = g_power_config.maintenance_interval_minutes - window_position;
        
        sleep_duration_sec = minutes_until_next_window * 60;
        
        ESP_LOGI(TAG, "💤 Sleeping for %llu seconds (%d min) until next maintenance window", 
                 sleep_duration_sec, minutes_until_next_window);
    } else {
        // --- SMART MULTI-TRIP WAKE UP ---
        uint64_t min_sleep_sec = 86400; // Default: Max 24 hours
        bool found_next = false;
        
        // Use multi-trip windows if available, otherwise fallback to legacy
        int count = (g_power_config.use_multi_trip) ? g_power_config.trip_window_count : 1;
        
        for (int i = 0; i < count; i++) {
            int start_h, start_m;
            if (g_power_config.use_multi_trip) {
                if (!g_power_config.trip_windows[i].active) continue;
                start_h = g_power_config.trip_windows[i].start_hour;
                start_m = g_power_config.trip_windows[i].start_minute;
            } else {
                start_h = g_power_config.trip_start_hour;
                start_m = g_power_config.trip_start_minute;
            }
            
            struct tm next_trip = timeinfo;
            next_trip.tm_hour = start_h;
            next_trip.tm_min = start_m;
            next_trip.tm_sec = 0;
            
            time_t next_trip_time = mktime(&next_trip);
            if (next_trip_time <= now) {
                next_trip.tm_mday += 1; // Try tomorrow
                next_trip_time = mktime(&next_trip);
            }
            
            uint64_t diff = (uint64_t)(next_trip_time - now);
            if (diff < min_sleep_sec) {
                min_sleep_sec = diff;
                found_next = true;
            }
        }
        
        sleep_duration_sec = min_sleep_sec;
        ESP_LOGI(TAG, "💤 Multi-Trip Sleep: Next wake up in %llu seconds", sleep_duration_sec);
        ESP_LOGI(TAG, "⏰ Wake up target: %02llu hours, %02llu minutes from now", 
                 sleep_duration_sec / 3600, (sleep_duration_sec % 3600) / 60);
    }
    
    // Give system time to finish current operations
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Configure wake up timer
    esp_sleep_enable_timer_wakeup(sleep_duration_sec * 1000000ULL);
    
    // Enter deep sleep
    esp_deep_sleep_start();
}

static void power_management_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🔋 Power management task started");
    
    // STARTUP GRACE PERIOD: Wait 30 seconds before allowing ANY deep sleep
    // This ensures that even if sync is slow, we don't sleep immediately
    ESP_LOGI(TAG, "🔍 Startup grace period: Staying awake for 30s...");
    vTaskDelay(pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "✅ Startup grace period ended, active monitoring starting");

    ESP_LOGI(TAG, "⏱️ Check intervals: Trip=%ds, Idle=%ds, Maintenance=%ds, Log=%ds",
             g_power_config.trip_check_interval_sec,
             g_power_config.idle_check_interval_sec,
             g_power_config.maintenance_check_interval_sec,
             g_power_config.log_interval_sec);
    
    bool maintenance_was_active = false;
    static bool trip_was_active = false;
    static time_t last_status_log = 0;
    
    while (1) {
        // Keep LED OFF at all times to save battery
        gpio_set_level(LED_GPIO, 0);
        
        // Check if it's trip time or maintenance window
        bool trip_active = is_trip_time();
        bool maintenance_active = is_maintenance_window();
        time_t current_time = time(NULL);
        
        if (trip_active) {
            // Ensure WiFi is in normal mode during trip
            esp_wifi_set_ps(WIFI_PS_NONE);
            
            // Log only on state change or at intervals
            if (!trip_was_active || (current_time - last_status_log >= g_power_config.log_interval_sec)) {
                ESP_LOGI(TAG, "🟢 Trip time active - system staying awake");
                last_status_log = current_time;
            }
            trip_was_active = true;
            
            // Update system health during active hours
            g_system_health.free_heap = esp_get_free_heap_size();
            
            // Use configurable interval for trip time checks
            vTaskDelay(pdMS_TO_TICKS(g_power_config.trip_check_interval_sec * 1000));
            
        } else if (maintenance_active) {
            // Maintenance window active
            if (!maintenance_was_active) {
                ESP_LOGI(TAG, "🔧 MAINTENANCE WINDOW ACTIVATED");
                esp_wifi_set_ps(WIFI_PS_NONE);
                ESP_LOGI(TAG, "📶 WiFi power saving DISABLED");
                maintenance_was_active = true;
                last_status_log = current_time;
            }
            
            // Log at intervals so user knows WHY it is awake
            if (current_time - last_status_log >= 60) {
                ESP_LOGI(TAG, "🔧 AWAKE FOR MAINTENANCE: Staying awake for %d mins of service", g_power_config.maintenance_duration_minutes);
                last_status_log = current_time;
            }
            
            trip_was_active = false;
            
            // Use configurable interval for maintenance checks
            vTaskDelay(pdMS_TO_TICKS(g_power_config.maintenance_check_interval_sec * 1000));
            
        } else {
            // Outside both trip and maintenance
            if (maintenance_was_active) {
                ESP_LOGI(TAG, "🔧 MAINTENANCE WINDOW ENDED");
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                ESP_LOGI(TAG, "📶 WiFi power saving RE-ENABLED");
                maintenance_was_active = false;
                last_status_log = current_time;
            }
            
            if (trip_was_active) {
                ESP_LOGI(TAG, "🔴 Trip time ended - entering idle mode");
                trip_was_active = false;
                last_status_log = current_time;
            }
            
            // Log deep sleep status only at intervals, not every check
            if (current_time - last_status_log >= g_power_config.log_interval_sec) {
                ESP_LOGI(TAG, "🔍 Deep sleep check: enable_deep_sleep=%s", 
                         g_power_config.enable_deep_sleep ? "true" : "false");
                last_status_log = current_time;
            }
            
            if (g_power_config.enable_deep_sleep) {
                ESP_LOGI(TAG, "🔴 Outside active hours - entering deep sleep");
                enter_deep_sleep();
            } else {
                // Only log occasionally when staying awake
                if (current_time - last_status_log >= g_power_config.log_interval_sec) {
                    ESP_LOGI(TAG, "⏸️ Outside active hours - deep sleep disabled, staying awake");
                }
                
                // Use much longer configurable interval when idle
                vTaskDelay(pdMS_TO_TICKS(g_power_config.idle_check_interval_sec * 1000));
            }
        }
    }
}

// NVS functions removed - config comes from server only


esp_err_t power_management_init(void)
{
    ESP_LOGI(TAG, "🔋 Initializing power management system");
    
    // Disable deep sleep hold first (in case we just woke up)
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(LED_GPIO);
    
    // Initialize LED GPIO and turn it OFF to save battery
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LED_GPIO, 0);  // Set twice to ensure it's off
    ESP_LOGI(TAG, "💡 LED GPIO initialized and turned OFF");
    
    // NVS loading disabled - config comes from server via power_config_sync
    
    // Initialize system health
    g_system_health.free_heap = esp_get_free_heap_size();
    g_system_health.system_healthy = true;
    
    ESP_LOGI(TAG, "📅 Trip hours: %02d:%02d - %02d:%02d", 
             g_power_config.trip_start_hour, g_power_config.trip_start_minute,
             g_power_config.trip_end_hour, g_power_config.trip_end_minute);
    
    if (g_power_config.enable_maintenance_windows) {
        ESP_LOGI(TAG, "🔧 Maintenance windows: Every %d min, Duration: %d min",
                 g_power_config.maintenance_interval_minutes,
                 g_power_config.maintenance_duration_minutes);
    }
    
    ESP_LOGI(TAG, "💤 Deep sleep: %s", g_power_config.enable_deep_sleep ? "ENABLED" : "DISABLED");
    
    // Show current time immediately on startup
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    bool trip_active = is_trip_time();
    ESP_LOGI(TAG, "🕐 STARTUP Time Check: Current=%02d:%02d, Trip=%02d:%02d-%02d:%02d, InTrip=%s", 
             timeinfo.tm_hour, timeinfo.tm_min,
             g_power_config.trip_start_hour, g_power_config.trip_start_minute,
             g_power_config.trip_end_hour, g_power_config.trip_end_minute,
             trip_active ? "YES" : "NO");
    
    // Create power management task
    // Reduced stack from 4096 to 3072 to save RAM for power sync
    BaseType_t result = xTaskCreate(
        power_management_task, 
        "power_mgmt", 
        3072, 
        NULL, 
        2,
        &g_power_task_handle
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "✅ Power management task created");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "❌ Failed to create power management task");
        return ESP_FAIL;
    }
}

// Public functions for other modules to report health status
void power_mgmt_report_wifi_reconnect(void)
{
    g_system_health.wifi_reconnects++;
}

void power_mgmt_report_camera_error(void)
{
    g_system_health.camera_errors++;
}

void power_mgmt_report_upload_failure(void)
{
    g_system_health.upload_failures++;
}

void power_mgmt_report_successful_upload(void)
{
    time(&g_system_health.last_successful_upload);
}

// power_mgmt_force_sleep removed - unused

void power_mgmt_disable_sleep(void)
{
    ESP_LOGI(TAG, "🔧 power_mgmt_disable_sleep() called");
    g_power_config.enable_deep_sleep = false;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("power_mgmt", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, "deep_sleep", 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    ESP_LOGI(TAG, "❌ Deep sleep DISABLED via web interface");
}

void power_mgmt_enable_sleep(void)
{
    ESP_LOGI(TAG, "🔧 power_mgmt_enable_sleep() called");
    g_power_config.enable_deep_sleep = true;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("power_mgmt", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, "deep_sleep", 1);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    ESP_LOGI(TAG, "✅ Deep sleep ENABLED via web interface");
}

bool power_mgmt_is_sleep_enabled(void)
{
    ESP_LOGI(TAG, "🔍 power_mgmt_is_sleep_enabled() called, returning: %d", g_power_config.enable_deep_sleep);
    return g_power_config.enable_deep_sleep;
}

bool power_mgmt_is_trip_time(void)
{
    return is_trip_time();
}

// power_mgmt_get_schedule removed - unused

esp_err_t power_mgmt_update_schedule(int start_hour, int start_min, int end_hour, int end_min)
{
    g_power_config.trip_start_hour = start_hour;
    g_power_config.trip_start_minute = start_min;
    g_power_config.trip_end_hour = end_hour;
    g_power_config.trip_end_minute = end_min;
    
    ESP_LOGI(TAG, "📅 Schedule updated: %02d:%02d - %02d:%02d", 
             start_hour, start_min, end_hour, end_min);
    
    // NVS save removed - config comes from server
    
    return ESP_OK;
}

void power_mgmt_enable_maintenance_windows(int interval_minutes, int duration_minutes)
{
    // If interval is 0, disable maintenance windows completely
    if (interval_minutes == 0) {
        ESP_LOGI(TAG, "🔧 Disabling maintenance windows (interval=0)");
        g_power_config.enable_maintenance_windows = false;
        g_power_config.maintenance_interval_minutes = 0;
        g_power_config.maintenance_duration_minutes = 0;
        
        // Save to NVS
        nvs_handle_t nvs_handle;
        if (nvs_open("power_mgmt", NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_i32(nvs_handle, "maint_enable", 0);
            nvs_set_i32(nvs_handle, "maint_interval", 0);
            nvs_set_i32(nvs_handle, "maint_duration", 0);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
        
        ESP_LOGI(TAG, "✅ Maintenance windows DISABLED");
        return;
    }
    
    ESP_LOGI(TAG, "🔧 Enabling maintenance windows: %d min interval, %d min duration", 
             interval_minutes, duration_minutes);
    
    g_power_config.enable_maintenance_windows = true;
    g_power_config.maintenance_interval_minutes = interval_minutes;
    g_power_config.maintenance_duration_minutes = duration_minutes;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("power_mgmt", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, "maint_enable", 1);
        nvs_set_i32(nvs_handle, "maint_interval", interval_minutes);
        nvs_set_i32(nvs_handle, "maint_duration", duration_minutes);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    ESP_LOGI(TAG, "✅ Maintenance windows enabled successfully");
}

esp_err_t power_mgmt_set_check_intervals(int trip_check_sec, int idle_check_sec, int maintenance_check_sec, int log_interval_sec)
{
    // Validate intervals (minimum 10 seconds, maximum 1 hour)
    if (trip_check_sec < 10 || trip_check_sec > 3600 ||
        idle_check_sec < 10 || idle_check_sec > 3600 ||
        maintenance_check_sec < 10 || maintenance_check_sec > 3600 ||
        log_interval_sec < 30 || log_interval_sec > 7200) {
        ESP_LOGE(TAG, "❌ Invalid intervals: must be 10-3600s for checks, 30-7200s for logging");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "⏱️ Setting check intervals: Trip=%ds, Idle=%ds, Maintenance=%ds, Log=%ds",
             trip_check_sec, idle_check_sec, maintenance_check_sec, log_interval_sec);
    
    g_power_config.trip_check_interval_sec = trip_check_sec;
    g_power_config.idle_check_interval_sec = idle_check_sec;
    g_power_config.maintenance_check_interval_sec = maintenance_check_sec;
    g_power_config.log_interval_sec = log_interval_sec;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("power_mgmt", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, "trip_check_int", trip_check_sec);
        nvs_set_i32(nvs_handle, "idle_check_int", idle_check_sec);
        nvs_set_i32(nvs_handle, "maint_check_int", maintenance_check_sec);
        nvs_set_i32(nvs_handle, "log_interval", log_interval_sec);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 Check intervals saved to NVS");
    } else {
        ESP_LOGE(TAG, "❌ Failed to save check intervals to NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    return ESP_OK;
}

esp_err_t power_mgmt_set_normal_intervals(void)
{
    ESP_LOGI(TAG, "📊 Setting NORMAL intervals for production");
    return power_mgmt_set_check_intervals(60, 300, 30, 300); // 1min, 5min, 30s, 5min
}

// Removed unused: power_mgmt_get_check_intervals, power_mgmt_set_quick_intervals, power_mgmt_set_slow_intervals
// Removed unused: power_mgmt_get_schedule_hours, power_mgmt_get_maintenance_settings

// Multi-trip support functions
esp_err_t power_mgmt_set_multi_trip_windows(trip_window_t* windows, int count)
{
    if (!windows || count <= 0 || count > MAX_TRIP_WINDOWS) {
        ESP_LOGE(TAG, "❌ Invalid trip windows: count=%d (max=%d)", count, MAX_TRIP_WINDOWS);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if manual override is active - if so, reject server updates
    if (g_power_config.manual_override) {
        ESP_LOGW(TAG, "⚠️ Manual override active - ignoring trip window update");
        return ESP_OK;  // Return success but don't update
    }
    
    ESP_LOGI(TAG, "📅 Setting %d trip windows:", count);
    
    g_power_config.use_multi_trip = true;
    g_power_config.trip_window_count = count;
    
    for (int i = 0; i < count; i++) {
        g_power_config.trip_windows[i] = windows[i];
        ESP_LOGI(TAG, "   %d. %s: %02d:%02d - %02d:%02d", 
                i + 1,
                windows[i].trip_name,
                windows[i].start_hour, windows[i].start_minute,
                windows[i].end_hour, windows[i].end_minute);
    }
    
    // NVS save removed - config comes from server
    
    return ESP_OK;
}

// Removed unused: power_mgmt_disable_multi_trip, power_mgmt_is_multi_trip_enabled, power_mgmt_get_trip_window_count


void power_mgmt_set_manual_override(bool enable)
{
    g_power_config.manual_override = enable;
    if (enable) {
        ESP_LOGI(TAG, "🔒 Manual override ENABLED - server updates will be ignored");
    } else {
        ESP_LOGI(TAG, "🔓 Manual override DISABLED - server updates allowed");
    }
}
