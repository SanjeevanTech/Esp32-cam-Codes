#include "gps_neo7m.hpp"
#include "gps_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "GPS_NEO7M";

static gps_config_t g_gps_config;
static gps_data_t g_gps_data;
static TaskHandle_t g_gps_task_handle = NULL;
static bool g_gps_initialized = false;

// NMEA parsing functions
static float nmea_to_decimal(const char *nmea_coord, char direction) {
    if (!nmea_coord || strlen(nmea_coord) < 4) return 0.0;
    
    // Parse DDMM.MMMM format
    float degrees = 0.0;
    float minutes = 0.0;
    
    // Find decimal point
    char *dot = strchr(nmea_coord, '.');
    if (!dot) return 0.0;
    
    // Extract degrees (first 2 or 3 digits before minutes)
    int deg_len = (dot - nmea_coord) - 2;
    if (deg_len > 0) {
        char deg_str[4] = {0};
        strncpy(deg_str, nmea_coord, deg_len);
        degrees = atof(deg_str);
    }
    
    // Extract minutes (last 2 digits before decimal + decimal part)
    char min_str[16] = {0};
    strncpy(min_str, nmea_coord + deg_len, dot - nmea_coord - deg_len + strlen(dot));
    minutes = atof(min_str);
    
    float decimal = degrees + (minutes / 60.0);
    
    // Apply direction
    if (direction == 'S' || direction == 'W') {
        decimal = -decimal;
    }
    
    return decimal;
}

static bool parse_gpgga(const char *sentence) {
    // $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
    if (!sentence) return false;
    
    char *tokens[15];
    char sentence_copy[256];
    strncpy(sentence_copy, sentence, sizeof(sentence_copy) - 1);
    sentence_copy[sizeof(sentence_copy) - 1] = '\0';
    
    // Initialize all tokens to NULL for safety
    memset(tokens, 0, sizeof(tokens));
    
    // Split by comma
    int token_count = 0;
    char *token = strtok(sentence_copy, ",");
    while (token && token_count < 15) {
        tokens[token_count++] = token;
        token = strtok(NULL, ",");
    }
    
    if (token_count < 7) return false;  // Need at least 7 tokens for fix quality
    
    // Check fix quality - validate token exists and is not empty
    if (!tokens[6] || strlen(tokens[6]) == 0) {
        g_gps_data.valid = false;
        return false;
    }
    
    int fix_quality = atoi(tokens[6]);
    if (fix_quality == 0) {
        g_gps_data.valid = false;
        return false;
    }
    
    // Parse latitude - validate tokens exist
    if (tokens[2] && tokens[3] && strlen(tokens[2]) > 0 && strlen(tokens[3]) > 0) {
        g_gps_data.latitude = nmea_to_decimal(tokens[2], tokens[3][0]);
    }
    
    // Parse longitude - validate tokens exist
    if (tokens[4] && tokens[5] && strlen(tokens[4]) > 0 && strlen(tokens[5]) > 0) {
        g_gps_data.longitude = nmea_to_decimal(tokens[4], tokens[5][0]);
    }
    
    // Parse satellites - validate token exists
    if (token_count > 7 && tokens[7] && strlen(tokens[7]) > 0) {
        g_gps_data.satellites = atoi(tokens[7]);
    }
    
    // Parse altitude - validate token exists
    if (token_count > 9 && tokens[9] && strlen(tokens[9]) > 0) {
        g_gps_data.altitude = atof(tokens[9]);
    }
    
    g_gps_data.valid = true;
    
    // Update timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(g_gps_data.timestamp, sizeof(g_gps_data.timestamp), 
             "%ld.%06ld", tv.tv_sec, tv.tv_usec);
    
    return true;
}

static bool parse_gprmc(const char *sentence) {
    // $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
    if (!sentence) return false;
    
    char *tokens[13];
    char sentence_copy[256];
    strncpy(sentence_copy, sentence, sizeof(sentence_copy) - 1);
    sentence_copy[sizeof(sentence_copy) - 1] = '\0';
    
    // Initialize all tokens to NULL for safety
    memset(tokens, 0, sizeof(tokens));
    
    // Split by comma
    int token_count = 0;
    char *token = strtok(sentence_copy, ",");
    while (token && token_count < 13) {
        tokens[token_count++] = token;
        token = strtok(NULL, ",");
    }
    
    if (token_count < 10) return false;  // Need at least 10 tokens for date/time
    
    // Check status - validate token exists
    if (!tokens[2] || strlen(tokens[2]) == 0 || tokens[2][0] != 'A') {
        g_gps_data.valid = false;
        return false;
    }
    
    // Parse latitude - validate tokens exist
    if (tokens[3] && tokens[4] && strlen(tokens[3]) > 0 && strlen(tokens[4]) > 0) {
        g_gps_data.latitude = nmea_to_decimal(tokens[3], tokens[4][0]);
    }
    
    // Parse longitude - validate tokens exist
    if (tokens[5] && tokens[6] && strlen(tokens[5]) > 0 && strlen(tokens[6]) > 0) {
        g_gps_data.longitude = nmea_to_decimal(tokens[5], tokens[6][0]);
    }
    
    g_gps_data.valid = true;

    // --- TIME SYNC FROM GPS ---
    // Token 1: Time (HHMMSS), Token 9: Date (DDMMYY)
    if (tokens[1] && tokens[9] && strlen(tokens[1]) >= 6 && strlen(tokens[9]) >= 6) {
        struct tm tm_gps;
        memset(&tm_gps, 0, sizeof(struct tm));
        char tmp[3] = {0};

        // Parse Time
        strncpy(tmp, tokens[1], 2); tm_gps.tm_hour = atoi(tmp);
        strncpy(tmp, tokens[1] + 2, 2); tm_gps.tm_min = atoi(tmp);
        strncpy(tmp, tokens[1] + 4, 2); tm_gps.tm_sec = atoi(tmp);

        // Parse Date
        strncpy(tmp, tokens[9], 2); tm_gps.tm_mday = atoi(tmp);
        strncpy(tmp, tokens[9] + 2, 2); tm_gps.tm_mon = atoi(tmp) - 1;
        strncpy(tmp, tokens[9] + 4, 2); tm_gps.tm_year = atoi(tmp) + 100; // Assume 20xx

        // mktime uses local timezone, but GPS provides UTC. 
        // We need to treat tm_gps as UTC to get correct unix timestamp.
        // On ESP-IDF, we can use setenv/tzset or a custom conversion.
        
        // Save current timezone
        char *prev_tz = getenv("TZ");
        char tz_backup[32] = {0};
        if (prev_tz) strncpy(tz_backup, prev_tz, sizeof(tz_backup)-1);
        
        // Set to UTC temporarily
        setenv("TZ", "UTC", 1);
        tzset();
        
        time_t gps_time = mktime(&tm_gps);
        
        // Restore timezone
        if (tz_backup[0]) setenv("TZ", tz_backup, 1);
        else unsetenv("TZ");
        tzset();

        if (gps_time > 1704067200) { // Valid time after 2024
            struct timeval now;
            gettimeofday(&now, NULL);
            // Only sync if system time is currently way off (before 2024)
            if (now.tv_sec < 1704067200) {
                struct timeval tv_gps = { .tv_sec = gps_time, .tv_usec = 0 };
                settimeofday(&tv_gps, NULL);
                ESP_LOGI(TAG, "?? System time SYNCED with GPS: %s %s", tokens[9], tokens[1]);
            }
        }
    }
    
    // Update data timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(g_gps_data.timestamp, sizeof(g_gps_data.timestamp), 
             "%ld.%06ld", tv.tv_sec, tv.tv_usec);
    
    return true;
}

static bool validate_nmea_checksum(const char *sentence) {
    if (!sentence || sentence[0] != '$') return false;
    
    // Find checksum
    char *asterisk = strrchr(sentence, '*');
    if (!asterisk) return false;
    
    // Calculate checksum
    uint8_t checksum = 0;
    for (const char *p = sentence + 1; p < asterisk; p++) {
        checksum ^= *p;
    }
    
    // Parse expected checksum
    uint8_t expected = strtol(asterisk + 1, NULL, 16);
    
    return checksum == expected;
}

static nmea_sentence_type_t get_sentence_type(const char *sentence) {
    if (strncmp(sentence, "$GPGGA", 6) == 0) return NMEA_GPGGA;
    if (strncmp(sentence, "$GPRMC", 6) == 0) return NMEA_GPRMC;
    if (strncmp(sentence, "$GPGSV", 6) == 0) return NMEA_GPGSV;
    return NMEA_UNKNOWN;
}

void gps_task(void *pvParameters) {
    char buffer[1024];  // Increased from 512 to handle both NEO-6M and NEO-7M
    int buffer_pos = 0;
    uint8_t data[256];  // Increased from 128 for faster processing
    
    ESP_LOGI(TAG, "GPS task started");
    
    // Safety check - ensure UART is properly initialized
    if (!g_gps_initialized) {
        ESP_LOGE(TAG, "GPS not initialized, stopping task");
        vTaskDelete(NULL);
        return;
    }
    
    // Flush any existing data in UART buffer
    uart_flush(g_gps_config.uart_port);
    
    while (true) {
        int len = uart_read_bytes(g_gps_config.uart_port, data, sizeof(data) - 1, pdMS_TO_TICKS(50));
        
        if (len < 0) {
            ESP_LOGE(TAG, "UART read error: %d", len);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        if (len > 0) {
            data[len] = '\0';
            
            for (int i = 0; i < len; i++) {
                char c = data[i];
                
                // Reset buffer when new NMEA sentence starts
                if (c == '$') {
                    buffer_pos = 0;
                    buffer[buffer_pos++] = c;
                    continue;
                }
                
                if (c == '\n' || c == '\r') {
                    if (buffer_pos > 0) {
                        buffer[buffer_pos] = '\0';
                        
                        // Validate and parse NMEA sentence
                        if (validate_nmea_checksum(buffer)) {
                            nmea_sentence_type_t type = get_sentence_type(buffer);
                            
                            switch (type) {
                                case NMEA_GPGGA:
                                    if (parse_gpgga(buffer)) {
                                        ESP_LOGD(TAG, "GPGGA parsed: %.6f, %.6f, %d sats", 
                                                g_gps_data.latitude, g_gps_data.longitude, g_gps_data.satellites);
                                    }
                                    break;
                                    
                                case NMEA_GPRMC:
                                    if (parse_gprmc(buffer)) {
                                        ESP_LOGD(TAG, "GPRMC parsed: %.6f, %.6f", 
                                                g_gps_data.latitude, g_gps_data.longitude);
                                    }
                                    break;
                                    
                                default:
                                    break;
                            }
                        }
                        
                        buffer_pos = 0;
                    }
                } else if (buffer_pos < sizeof(buffer) - 1) {
                    buffer[buffer_pos++] = c;
                } else {
                    // Buffer overflow protection - flush UART and reset
                    static int64_t last_overflow_log = 0;
                    int64_t now = esp_timer_get_time();
                    if (now - last_overflow_log > 60000000) {  // Log max once per 60 seconds
                        ESP_LOGW(TAG, "GPS buffer overflow, flushing UART");
                        last_overflow_log = now;
                    }
                    uart_flush(g_gps_config.uart_port);  // Flush backed up data
                    buffer_pos = 0;
                }
            }
        }
        
        // Reduced delay for faster GPS data processing (was 50ms)
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" {

esp_err_t gps_init(gps_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&g_gps_config, config, sizeof(gps_config_t));
    
    // Initialize GPS data
    memset(&g_gps_data, 0, sizeof(gps_data_t));
    
    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = config->baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_APB;
    uart_config.rx_flow_ctrl_thresh = 0;
    
    esp_err_t ret = uart_driver_install(config->uart_port, 2048, 0, 0, NULL, 0);  // Increased from 1024
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_param_config(config->uart_port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_set_pin(config->uart_port, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        return ret;
    }
    
    g_gps_initialized = true;
    ESP_LOGI(TAG, "GPS initialized on UART%d (TX:%d, RX:%d, Baud:%d)", 
             config->uart_port, config->tx_pin, config->rx_pin, config->baud_rate);
    
    return ESP_OK;
}

esp_err_t gps_start(void) {
    if (!g_gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_gps_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE; // Already started
    }
    
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, &g_gps_task_handle);
    ESP_LOGI(TAG, "GPS task started");
    
    return ESP_OK;
}

esp_err_t gps_stop(void) {
    if (g_gps_task_handle != NULL) {
        vTaskDelete(g_gps_task_handle);
        g_gps_task_handle = NULL;
        ESP_LOGI(TAG, "GPS task stopped");
    }
    
    return ESP_OK;
}

gps_data_t gps_get_current_data(void) {
    return g_gps_data;
}

bool gps_is_valid(void) {
    return g_gps_data.valid;
}

} // extern "C"