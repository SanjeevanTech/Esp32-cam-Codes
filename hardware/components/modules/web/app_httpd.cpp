// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "app_httpd.hpp"

#include <list>
#include <cstdlib>  // For strtol function used in URL decoding
#include <cstring>  // For strlen function used in URL decoding
#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"
// #include "fb_gfx.h" // Removed to save IRAM
#include "app_mdns.h"
#include "sdkconfig.h"

#include "who_camera.h"

// Forward declarations for GPS functions
extern "C" {
    typedef struct {
        float latitude;
        float longitude;
        float altitude;
        int satellites;
        bool valid;
        char timestamp[32];
    } gps_data_t;
    
    gps_data_t gps_get_current_data(void);
    bool gps_is_valid(void);
    
    // Power management functions
    void power_mgmt_enable_sleep(void);
    void power_mgmt_disable_sleep(void);
    bool power_mgmt_is_sleep_enabled(void);
}

// Power settings struct removed - settings are managed centrally via Python server
#pragma GCC diagnostic pop

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define TAG ""
#else
#include "esp_log.h"
static const char *TAG = "camera_httpd";
#endif

static QueueHandle_t xQueueFrameI = NULL;
static QueueHandle_t xQueueFrameO = NULL;
static bool gReturnFB = true;

// Forward declaration for memory logging function
void log_memory_usage(const char* location);

// URL decoding function removed - no longer needed

static int8_t detection_enabled = 1;
static int8_t recognition_enabled = 1;
static int8_t is_enrolling = 0;
// stream_enabled removed - video streaming disabled to save memory

#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
#include "driver/ledc.h"
#define CONFIG_LED_LEDC_CHANNEL LEDC_CHANNEL_2  // Use channel 2 for LED
#define CONFIG_LED_MAX_INTENSITY 255

static int led_duty = 0;
static bool isStreaming = false;

// LED Flash control functions
void enable_led(bool en)
{
    int duty = en ? led_duty : 0;
    if (duty > CONFIG_LED_MAX_INTENSITY) {
        duty = CONFIG_LED_MAX_INTENSITY;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL);
    ESP_LOGI(TAG, "LED Flash: %s (duty: %d)", en ? "ON" : "OFF", duty);
}
#endif

// Suppress false positive unused warnings - these variables are used in control handlers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"


// httpd_handle_t stream_httpd = NULL;  // Removed to save memory
httpd_handle_t camera_httpd = NULL;



static esp_err_t capture_handler(httpd_req_t *req)
{
    log_memory_usage("capture_handler START");
    camera_fb_t *frame = NULL;
    esp_err_t res = ESP_OK;
    
#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
    // Turn on LED flash for capture
    enable_led(true);
#endif
    
    // Check available memory before processing
    uint32_t free_heap = esp_get_free_heap_size();
    if (free_heap < 100000) {  // Less than 100KB free
        ESP_LOGW(TAG, "⚠️ Low memory for capture: %d bytes", free_heap);
#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
        enable_led(false);
#endif
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Use shorter timeout to prevent hanging
    if (xQueueReceive(xQueueFrameI, &frame, pdMS_TO_TICKS(5000)))  // 5 second timeout
    {
        if (frame && frame->buf && frame->len > 0)
        {
            log_memory_usage("capture_handler FRAME_RECEIVED");
            
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

            char ts[32];
            snprintf(ts, 32, "%ld.%06ld", frame->timestamp.tv_sec, frame->timestamp.tv_usec);
            httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

            if (frame->format == PIXFORMAT_JPEG)
            {
                // Direct JPEG send - most memory efficient
                log_memory_usage("capture_handler BEFORE_JPEG_SEND");
                res = httpd_resp_send(req, (const char *)frame->buf, frame->len);
                log_memory_usage("capture_handler AFTER_JPEG_SEND");
            }
            else
            {
                // Convert to JPEG - more memory intensive
                log_memory_usage("capture_handler BEFORE_CONVERSION");
                
                // Check memory again before conversion
                free_heap = esp_get_free_heap_size();
                if (free_heap < 150000) {  // Need more memory for conversion
                    ESP_LOGW(TAG, "⚠️ Insufficient memory for JPEG conversion: %d bytes", free_heap);
                    res = ESP_FAIL;
                } else {
                    // Convert to JPEG using simpler method (no streaming)
                    uint8_t *jpg_buf = NULL;
                    size_t jpg_len = 0;
                    if (frame2jpg(frame, 80, &jpg_buf, &jpg_len)) {
                        res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
                        free(jpg_buf);  // Free the allocated JPEG buffer
                    } else {
                        res = ESP_FAIL;
                    }
                }
                
                log_memory_usage("capture_handler AFTER_CONVERSION");
            }

            // Always return frame to prevent memory leak
            if (xQueueFrameO)
            {
                xQueueSend(xQueueFrameO, &frame, pdMS_TO_TICKS(1000));  // 1 second timeout
            }
            else if (gReturnFB)
            {
                esp_camera_fb_return(frame);
            }
            else
            {
                free(frame);
            }
            
            log_memory_usage("capture_handler FRAME_RETURNED");
        }
        else
        {
            ESP_LOGE(TAG, "❌ Invalid frame received (null or empty)");
            res = ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "❌ Camera capture timeout - no frame received");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Capture handler failed");
        httpd_resp_send_500(req);
    }
    
#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
    // Turn off LED flash after capture
    enable_led(false);
#endif
    
    log_memory_usage("capture_handler END");
    return res;
}

// Video streaming completely removed to save memory and bandwidth
// Only photo capture available via /capture endpoint

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = (char *)malloc(buf_len);
        if (!buf)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK ||
        httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    ESP_LOGI(TAG, "%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize"))
    {
        if (s->pixformat == PIXFORMAT_JPEG)
        {
            res = s->set_framesize(s, (framesize_t)val);
            if (res == 0)
            {
                app_mdns_update_framesize(val);
            }
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
    {
        // Horizontal mirror removed to save memory and simplify interface
        ESP_LOGI(TAG, "Horizontal mirror control disabled to save memory");
    }
    else if (!strcmp(variable, "vflip"))
    {
        // Vertical flip removed to save memory and simplify interface  
        ESP_LOGI(TAG, "Vertical flip control disabled to save memory");
    }
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);
#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
    else if (!strcmp(variable, "led_intensity"))
        led_duty = val;
#endif

    else if (!strcmp(variable, "face_detect"))
    {
        detection_enabled = val;
        if (!detection_enabled)
        {
            recognition_enabled = 0;
        }
    }
    else if (!strcmp(variable, "face_enroll"))
        is_enrolling = val;
    else if (!strcmp(variable, "face_recognize"))
    {
        recognition_enabled = val;
        if (recognition_enabled)
        {
            detection_enabled = val;
        }
    }
    // stream_enabled control completely removed - no streaming functionality
    else
    {
        res = -1;
    }

    if (res)
    {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask)
{
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[1024];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID)
    {
        for (int reg = 0x3400; reg < 0x3406; reg += 2)
        {
            p += print_reg(p, s, reg, 0xFFF); //12 bit
        }
        p += print_reg(p, s, 0x3406, 0xFF);

        p += print_reg(p, s, 0x3500, 0xFFFF0); //16 bit
        p += print_reg(p, s, 0x3503, 0xFF);
        p += print_reg(p, s, 0x350a, 0x3FF);  //10 bit
        p += print_reg(p, s, 0x350c, 0xFFFF); //16 bit

        for (int reg = 0x5480; reg <= 0x5490; reg++)
        {
            p += print_reg(p, s, reg, 0xFF);
        }

        for (int reg = 0x5380; reg <= 0x538b; reg++)
        {
            p += print_reg(p, s, reg, 0xFF);
        }

        for (int reg = 0x5580; reg < 0x558a; reg++)
        {
            p += print_reg(p, s, reg, 0xFF);
        }
        p += print_reg(p, s, 0x558a, 0x1FF); //9 bit
    }
    else
    {
        p += print_reg(p, s, 0xd3, 0xFF);
        p += print_reg(p, s, 0x111, 0xFF);
        p += print_reg(p, s, 0x132, 0xFF);
    }

    p += sprintf(p, "\"board\":\"%s\",", CAMERA_MODULE_NAME);
    p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
    p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    // hmirror removed to save memory and simplify interface
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
    p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
    p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
    p += sprintf(p, ",\"face_detect\":%u", detection_enabled);
    p += sprintf(p, ",\"face_enroll\":%u,", is_enrolling);
    p += sprintf(p, "\"face_recognize\":%u,", recognition_enabled);
    // stream_enabled removed from status - no streaming functionality
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

// mdns_handler removed - unused function

// Memory logging function
void log_memory_usage(const char* location) {
    ESP_LOGI(TAG, "[%s] Free heap: %d bytes, Min: %d bytes", 
             location,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size());
}

// Unused handlers and functions removed to save memory

static esp_err_t index_handler(httpd_req_t *req)
{
    log_memory_usage("index_handler START");
    
    // HTML files removed to save memory - using React frontend instead
    // Return simple redirect message to React interface
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    const char* response = "{"
        "\"status\":\"success\","
        "\"message\":\"ESP32 Camera API - HTML interface removed to save memory\","
        "\"note\":\"Use React frontend at http://localhost:5173 for full interface\","
        "\"api_endpoints\":["
            "\"/status - Camera status\","
            "\"/gps - GPS location\","
            "\"/control - Camera control\","
            "\"/power/settings - Power settings\","
            "\"/capture - Capture photo\""
        "]"
    "}";
    
    log_memory_usage("index_handler AFTER MEMORY SAVE");
    return httpd_resp_send(req, response, strlen(response));
}

// monitor_handler REMOVED - unused function

// OLD POWER MANAGEMENT HANDLERS REMOVED
// Use centralized React frontend at http://server:8888 instead
// These handlers are no longer needed and have been removed to save memory

// test_toggle_handler removed - unused function that was causing compiler warning

// load_power_settings_from_power_mgmt removed - settings managed centrally via Python server

void register_httpd(const QueueHandle_t frame_i, const QueueHandle_t frame_o, const bool return_fb)
{
    xQueueFrameI = frame_i;
    xQueueFrameO = frame_o;
    gReturnFB = return_fb;
    
    // Power settings managed centrally via Python server - no local loading needed

#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
    // Initialize LED flash
    led_duty = 0;  // Start with LED off
    
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    
    ledc_channel_config_t ledc_channel = {
        .gpio_num = GPIO_NUM_4,  // GPIO 4 is the flash LED on ESP32-CAM
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CONFIG_LED_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
    
    ESP_LOGI(TAG, "💡 LED Flash initialized on GPIO 4");
#endif

    // HTTP server DISABLED to save memory for face recognition
    // Face logs are sent via csv_uploader to Python backend instead
    ESP_LOGI(TAG, "⚠️ HTTP server DISABLED to save memory for face recognition");
    ESP_LOGI(TAG, "📷 Use external backend (Node.js/Python) to access camera");
    
    // To enable HTTP server, uncomment the httpd_start code below
    // and ensure you have enough free heap (>50KB)
}
