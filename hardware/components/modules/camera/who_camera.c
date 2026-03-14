#include "who_camera.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "who_camera";
static QueueHandle_t xQueueFrameO = NULL;

static void task_process_handler(void *arg)
{
    ESP_LOGI(TAG, "📷 Camera task started on core %d", xPortGetCoreID());
    int frame_count = 0;
    int frame_success = 0;
    int frame_dropped = 0;
    int consecutive_failures = 0;
    int failure_count = 0;
    
    while (true)
    {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame) {
            frame_count++;
            frame_success++;
            consecutive_failures = 0; // Reset on success
            
            // Log every 100 frames to reduce spam
            if (frame_count % 100 == 0) {
                ESP_LOGI(TAG, "📷 Captured %d frames (success: %d, dropped: %d), size: %d bytes", 
                         frame_count, frame_success, frame_dropped, frame->len);
            }
            
            // Try to send frame, but don't block forever if queue is full
            if (xQueueSend(xQueueFrameO, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
                // Queue full - drop oldest frame and try again
                camera_fb_t *old_frame = NULL;
                if (xQueueReceive(xQueueFrameO, &old_frame, 0) == pdTRUE) {
                    esp_camera_fb_return(old_frame);
                    if (xQueueSend(xQueueFrameO, &frame, 0) != pdTRUE) {
                        esp_camera_fb_return(frame);
                        frame_dropped++;
                    }
                } else {
                    esp_camera_fb_return(frame);
                    frame_dropped++;
                }
                
                // Only log every 10th drop to reduce spam
                if (frame_dropped % 10 == 0) {
                    ESP_LOGW(TAG, "Frame queue full, dropped %d frames", frame_dropped);
                }
            }
        } else {
            consecutive_failures++;
            failure_count++;
            
            // Only log every 10th failure to reduce spam
            if (failure_count % 10 == 0) {
                ESP_LOGW(TAG, "Failed to get frame from camera (consecutive: %d, total: %d)", 
                         consecutive_failures, failure_count);
            }
            
            // Adaptive delay based on consecutive failures
            if (consecutive_failures < 5) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else if (consecutive_failures < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}

void register_camera(const pixformat_t pixel_fromat,
                    const framesize_t frame_size,
                    const uint8_t fb_count,
                    const QueueHandle_t frame_o)
{
    ESP_LOGI(TAG, "Camera module is %s", CAMERA_MODULE_NAME);

#if CONFIG_CAMERA_MODULE_ESP_EYE || CONFIG_CAMERA_MODULE_ESP32_CAM_BOARD
    /* IO13, IO14 is designed for JTAG by default,
     * to use it as generalized input,
     * firstly declair it as pullup input */
    gpio_config_t conf;
    conf.mode = GPIO_MODE_INPUT;
    conf.pull_up_en = GPIO_PULLUP_ENABLE;
    conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    conf.intr_type = GPIO_INTR_DISABLE;
    conf.pin_bit_mask = 1LL << 13;
    gpio_config(&conf);
    conf.pin_bit_mask = 1LL << 14;
    gpio_config(&conf);
#endif

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sscb_sda = CAMERA_PIN_SIOD;
    config.pin_sscb_scl = CAMERA_PIN_SIOC;
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.xclk_freq_hz = 10000000; // 10 MHz - required for AI-Thinker OV2640 to fix FB-SIZE 149760 != 153600 error
    config.pixel_format = pixel_fromat;
    config.frame_size = frame_size;
    config.jpeg_quality = 30;
    config.fb_count = fb_count >= 3 ? fb_count : 3;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
    }
    
    // Verify actual frame size matches expected
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Failed to get sensor pointer after init");
        return;
    }
    
    // Force resolution to match what we requested
    s->set_framesize(s, frame_size);
    
    // Get actual resolution
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    if (info) {
        ESP_LOGI(TAG, "📷 Camera initialized: %s (max framesize=%d)", 
                 info->name, info->max_size);
    }
    
    // Test frame capture to verify buffer size
    camera_fb_t *test_fb = esp_camera_fb_get();
    if (test_fb) {
        ESP_LOGI(TAG, "📷 Frame buffer: %dx%d, %d bytes, format=%d", 
                 test_fb->width, test_fb->height, test_fb->len, test_fb->format);
        esp_camera_fb_return(test_fb);
    } else {
        ESP_LOGW(TAG, "⚠️ Could not capture test frame");
    }
    
    ESP_LOGI(TAG, "📷 Sensor detected: PID=0x%04X (OV2640=0x%04X, OV3660=0x%04X)", 
             s->id.PID, OV2640_PID, OV3660_PID);
    
    // Always flip vertically for AI-Thinker mounting orientation
    s->set_vflip(s, 1);
    // Force horizontal mirror OFF — both boards must produce identical spatial orientation
    // so that face_recognition_tool::align_face() landmark geometry is consistent
    s->set_hmirror(s, 0);
    
    if (s->id.PID == OV3660_PID)
    {
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
        ESP_LOGI(TAG, "📷 OV3660 configured with auto settings");
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
        ESP_LOGI(TAG, "📷 CAMERA SETTINGS DUMP:");
        ESP_LOGI(TAG, "  Sensor: OV3660 (PID=0x%04X)", s->id.PID);
        ESP_LOGI(TAG, "  Resolution: QVGA (320x240)");
        ESP_LOGI(TAG, "  Pixel Format: RGB565");
        ESP_LOGI(TAG, "  Frame Buffers: %d", fb_count);
        ESP_LOGI(TAG, "  vflip: 1, hmirror: 0");
        ESP_LOGI(TAG, "  brightness: 1, saturation: -2");
        ESP_LOGI(TAG, "  Other: AUTO mode (gain/exposure/whitebal enabled)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    }
    else if (s->id.PID == OV2640_PID)
    {
        // OV2640: Keep AUTO exposure/gain enabled for dynamic lighting inside the bus
        ESP_LOGI(TAG, "📷 OV2640 detected — enabling AUTO exposure/gain for dynamic lighting...");
        
        s->set_gain_ctrl(s, 1);       // ENABLE auto gain
        s->set_exposure_ctrl(s, 1);   // ENABLE auto exposure
        s->set_whitebal(s, 1);        // ENABLE auto white balance
        s->set_aec2(s, 1);            // Enable AEC2 for better exposure range
        
        // Other fixed settings for consistency
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_sharpness(s, 1);
        s->set_denoise(s, 1);
        s->set_lenc(s, 1);
        s->set_raw_gma(s, 1);
        
        ESP_LOGI(TAG, "📷 OV2640 configured with AUTO exposure/gain");
        
        // Print all camera settings for debugging
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
        ESP_LOGI(TAG, "📷 CAMERA SETTINGS DUMP:");
        ESP_LOGI(TAG, "  Sensor: OV2640 (PID=0x%04X)", s->id.PID);
        ESP_LOGI(TAG, "  Resolution: QVGA (320x240)");
        ESP_LOGI(TAG, "  Pixel Format: RGB565");
        ESP_LOGI(TAG, "  Frame Buffers: %d", fb_count);
        ESP_LOGI(TAG, "  vflip: 1, hmirror: 0");
        ESP_LOGI(TAG, "  gain_ctrl: 1 (AUTO)");
        ESP_LOGI(TAG, "  exposure_ctrl: 1 (AUTO)");
        ESP_LOGI(TAG, "  whitebal: 1 (AUTO)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    }
    else
    {
        // Unknown sensor: use safe auto settings
        ESP_LOGW(TAG, "⚠️ Unknown sensor PID=0x%04X — using AUTO settings", s->id.PID);
        s->set_gain_ctrl(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_whitebal(s, 1);
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
        ESP_LOGI(TAG, "📷 CAMERA SETTINGS DUMP:");
        ESP_LOGI(TAG, "  Sensor: UNKNOWN (PID=0x%04X)", s->id.PID);
        ESP_LOGI(TAG, "  Resolution: QVGA (320x240)");
        ESP_LOGI(TAG, "  Pixel Format: RGB565");
        ESP_LOGI(TAG, "  Frame Buffers: %d", fb_count);
        ESP_LOGI(TAG, "  vflip: 1, hmirror: 0");
        ESP_LOGI(TAG, "  Mode: AUTO (gain/exposure/whitebal enabled)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    }

    xQueueFrameO = frame_o;
    xTaskCreatePinnedToCore(task_process_handler, TAG, 3 * 1024, NULL, 6, NULL, 1);
}