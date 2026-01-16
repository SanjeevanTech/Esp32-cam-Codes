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
         config.xclk_freq_hz = 8000000; // 8 MHz for stability (reduced from 10 MHz)
         config.pixel_format = pixel_fromat;
         config.frame_size = frame_size;
         config.jpeg_quality = 30; // More compression
         config.fb_count = fb_count >= 3 ? fb_count : 3; // Minimum 3 buffers to prevent frame drops
         config.fb_location = CAMERA_FB_IN_PSRAM;
         config.grab_mode = CAMERA_GRAB_LATEST; // Skip old frames if processing is slow

         // camera init
         esp_err_t err = esp_camera_init(&config);
         if (err != ESP_OK)
         {
             ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
             return;
         }

         sensor_t *s = esp_camera_sensor_get();
         s->set_vflip(s, 1); // flip it back
         
         // Enhanced settings for better low-light face detection
         if (s->id.PID == OV3660_PID)
         {
             s->set_brightness(s, 1);  // up the brightness just a bit
             s->set_saturation(s, -2); // lower the saturation
         }
         else if (s->id.PID == OV2640_PID)
         {
             // OV2640 optimized for low-light face detection
             s->set_brightness(s, 2);      // Increase brightness (range: -2 to 2)
             s->set_contrast(s, 2);        // Increase contrast for better face edges
             s->set_saturation(s, 0);      // Normal saturation
             s->set_sharpness(s, 2);       // Sharper image for face features
             s->set_denoise(s, 1);         // Enable denoise for low light
             s->set_gainceiling(s, GAINCEILING_16X); // Higher gain for low light
             s->set_agc_gain(s, 30);       // Auto gain control (0-30, higher = brighter)
             s->set_aec_value(s, 1200);    // Auto exposure (0-1200, higher = brighter)
             s->set_aec2(s, 1);            // Enable automatic exposure control
             s->set_ae_level(s, 2);        // Exposure level (range: -2 to 2, higher = brighter)
             s->set_awb_gain(s, 1);        // Auto white balance gain
             s->set_wb_mode(s, 0);         // Auto white balance mode
             
             ESP_LOGI(TAG, "📷 Camera optimized for low-light face detection");
             ESP_LOGI(TAG, "   Brightness: +2, Contrast: +2, Gain: 16X");
         }

         xQueueFrameO = frame_o;
         xTaskCreatePinnedToCore(task_process_handler, TAG, 3 * 1024, NULL, 6, NULL, 1);
     }