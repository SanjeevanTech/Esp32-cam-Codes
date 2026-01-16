#include "who_human_face_recognition.hpp"

#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "img_converters.h"
#include <cmath>
#include "driver/gpio.h"

#include "dl_image.hpp"
// #include "fb_gfx.h" // Removed to save IRAM

#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "face_recognition_tool.hpp"
#include "../gps/gps_neo7m.hpp"
#include "csv_logger.h"
#include "csv_uploader.h"

// AI-THINKER ESP32-CAM LED pins
// Standard AI-THINKER has 2 LEDs:
// 1. White flash LED (bright, near camera) - GPIO 4
// 2. Red status LED (small, on back) - GPIO 33
#define LED_BUILTIN GPIO_NUM_33   // Red status LED on back
#define LED_FLASH GPIO_NUM_4      // White flash LED (bright)

#if CONFIG_MFN_V1
#if CONFIG_S8
#include "face_recognition_112_v1_s8.hpp"
#elif CONFIG_S16
#include "face_recognition_112_v1_s16.hpp"
#endif
#endif

#include "who_ai_utils.hpp"

using namespace std;
using namespace dl;

static const char *TAG = "human_face_recognition";

static QueueHandle_t xQueueFrameI = NULL;
static QueueHandle_t xQueueEvent = NULL;
static QueueHandle_t xQueueFrameO = NULL;
static QueueHandle_t xQueueResult = NULL;

static recognizer_state_t gEvent = RECOGNIZE;
static bool gReturnFB = true;
static face_info_t recognize_result;

SemaphoreHandle_t xMutex;

// Custom face recognition settings
static bool system_reset_flag = false;  // Flag for first run after reset - DISABLED to persist faces
static int stored_face_id = -1;        // Currently stored face ID
static const float SIMILARITY_THRESHOLD = 0.5f;  // Threshold for face matching (lowered to 0.5 for more tolerance)
static int64_t s_last_detection_us = 0;
static const int64_t DETECTION_THROTTLE_US = 500 * 1000; // 0.5 seconds between detections (much faster)

typedef struct RecPostArgs {
    uint8_t *jpeg_buf;
    size_t jpeg_len;
    int face_id;
    bool is_new_face;
} RecPostArgs;


typedef enum
{
    SHOW_STATE_IDLE,
    SHOW_STATE_DELETE,
    SHOW_STATE_RECOGNIZE,
    SHOW_STATE_ENROLL,
} show_state_t;

#define RGB565_MASK_RED 0xF800
#define RGB565_MASK_GREEN 0x07E0
#define RGB565_MASK_BLUE 0x001F
#define FRAME_DELAY_NUM 16


// Global variables to track LED state with cooldown
static esp_timer_handle_t s_led_timer = NULL;
static int64_t s_led_last_trigger_us = 0;
static const int64_t LED_DURATION_US = 1 * 1000 * 1000;  // 1 second LED duration
static const int64_t LED_COOLDOWN_US = 3 * 1000 * 1000;  // 3 seconds cooldown between flashes

// Timer callback to turn off LED (runs independently of main task)
static void led_off_timer_callback(void* arg)
{
    gpio_set_level(LED_FLASH, 0);
    ESP_LOGI(TAG, "💡 LEDs OFF");
}

// Function to turn on LEDs for 1 second when face is detected (NON-BLOCKING with cooldown)
static void flash_led_on_face_detect()
{
    int64_t now = esp_timer_get_time();
    
    // Only trigger LED if cooldown period has passed since last trigger
    if ((now - s_led_last_trigger_us) >= LED_COOLDOWN_US) {
        // Turn ON white LED
        gpio_set_level(LED_FLASH, 1);
        ESP_LOGI(TAG, "💡 LED ON - Face detected! (1 second flash)");
        
        // Start one-shot timer to turn off LED after exactly 1 second
        if (s_led_timer) {
            esp_timer_stop(s_led_timer);  // Stop any existing timer
            esp_timer_start_once(s_led_timer, LED_DURATION_US);  // Start new 1-second timer
        }
        
        s_led_last_trigger_us = now;
    }
    // If within cooldown, ignore this detection
}

static void task_process_handler(void *arg)
{
    camera_fb_t *frame = NULL;
    
    // Initialize red status LED (GPIO 33)
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LED_BUILTIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(LED_BUILTIN, 0);  // Start with LED OFF
    
    // Initialize white flash LED (GPIO 4)
    gpio_config_t flash_conf = {};
    flash_conf.intr_type = GPIO_INTR_DISABLE;
    flash_conf.mode = GPIO_MODE_OUTPUT;
    flash_conf.pin_bit_mask = (1ULL << LED_FLASH);
    flash_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    flash_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&flash_conf);
    gpio_set_level(LED_FLASH, 0);  // Start with flash LED OFF
    
    // Create one-shot timer for LED control (runs independently)
    esp_timer_create_args_t timer_args = {
        .callback = &led_off_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_off_timer"
    };
    esp_err_t timer_err = esp_timer_create(&timer_args, &s_led_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED timer: %s", esp_err_to_name(timer_err));
    } else {
        ESP_LOGI(TAG, "💡 LED timer created successfully");
    }
    
    ESP_LOGI(TAG, "🚀 Face detection task starting on core %d...", xPortGetCoreID());
    ESP_LOGI(TAG, "💡 Red LED initialized on GPIO %d", LED_BUILTIN);
    ESP_LOGI(TAG, "💡 White Flash LED initialized on GPIO %d", LED_FLASH);
    
    // Relaxed thresholds for better detection
    // resize_scale increased from 0.3 to 0.5 for larger detection area
    HumanFaceDetectMSR01 detector(0.25F, 0.3F, 10, 0.5F);  // Lower score, higher scale
    HumanFaceDetectMNP01 detector2(0.35F, 0.3F, 10);       // Lower score threshold
    
    ESP_LOGI(TAG, "📊 Detector config: MSR01(score=0.25, scale=0.5), MNP01(score=0.35)");

#if CONFIG_MFN_V1
#if CONFIG_S8
    FaceRecognition112V1S8 *recognizer = new FaceRecognition112V1S8();
#elif CONFIG_S16
    FaceRecognition112V1S16 *recognizer = new FaceRecognition112V1S16();
#endif
#endif
    show_state_t frame_show_state = SHOW_STATE_IDLE;
    recognizer_state_t _gEvent;
    recognizer->set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fr");
    
    // On system reset: clear all stored faces
    if (system_reset_flag) {
        ESP_LOGI(TAG, "System reset detected - clearing all stored faces");
        while (recognizer->get_enrolled_id_num() > 0) {
            recognizer->delete_id(true);  // Delete with flash update
        }
        stored_face_id = -1;
        system_reset_flag = false;
        ESP_LOGI(TAG, "All faces cleared, ready for new enrollment");
    } else {
        recognizer->set_ids_from_flash();
        if (recognizer->get_enrolled_id_num() > 0) {
            stored_face_id = recognizer->get_enrolled_ids().back().id;
            ESP_LOGI(TAG, "Loaded existing face ID: %d", stored_face_id);
        }
    }
    
    ESP_LOGI(TAG, "📊 Similarity threshold: %.2f", SIMILARITY_THRESHOLD);
    ESP_LOGI(TAG, "📊 Detection throttle: %lld seconds", DETECTION_THROTTLE_US / 1000000);
    ESP_LOGI(TAG, "📊 Waiting for frames from camera...");
    
    int process_count = 0;
    int faces_detected = 0;
    int faces_enrolled = 0;

    while (true)
    {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        _gEvent = RECOGNIZE;
        gEvent = RECOGNIZE;
        xSemaphoreGive(xMutex);

        if (_gEvent)
        {
            bool is_detected = false;

            if (xQueueReceive(xQueueFrameI, &frame, portMAX_DELAY))
            {
                process_count++;
                
                // Log every 10 frames
                if (process_count % 10 == 0) {
                    ESP_LOGI(TAG, "🔍 Processing frame %d (size: %dx%d, detected: %d, enrolled: %d)", 
                             process_count, frame->width, frame->height, faces_detected, faces_enrolled);
                }
                
                int64_t start_time = esp_timer_get_time();
                std::list<dl::detect::result_t> &detect_candidates = detector.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3});
                std::list<dl::detect::result_t> &detect_results = detector2.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_candidates);
                
                int64_t detection_time = (esp_timer_get_time() - start_time) / 1000;  // Convert to ms
                
                if (process_count % 10 == 0) {
                    ESP_LOGI(TAG, "⏱️ Detection took %lld ms, found %d faces", 
                             detection_time, detect_results.size());
                }

                if (detect_results.size() == 1) {
                    is_detected = true;
                    faces_detected++;
                    ESP_LOGI(TAG, "✅ Face detected in frame (total: %d)", faces_detected);
                    
                    // Turn on LED for 1 second when face is detected (NON-BLOCKING)
                    flash_led_on_face_detect();
                } else if (detect_results.size() > 1) {
                    ESP_LOGW(TAG, "Multiple faces detected (%d), ignoring", detect_results.size());
                } else {
                    // Only log at DEBUG level to reduce spam
                    ESP_LOGD(TAG, "No face detected in frame");
                }

                if (is_detected)
                {
                    // Get current GPS data for logging
                    gps_data_t current_gps = gps_get_current_data();
                    csv_gps_data_t csv_gps = {
                        .latitude = current_gps.latitude,
                        .longitude = current_gps.longitude,
                        .altitude = current_gps.altitude,
                        .satellites = current_gps.satellites,
                        .valid = current_gps.valid,
                        .timestamp = {0}
                    };
                    strncpy(csv_gps.timestamp, current_gps.timestamp, sizeof(csv_gps.timestamp) - 1);
                    csv_gps.timestamp[sizeof(csv_gps.timestamp) - 1] = '\0';

                    // Dummy embedding no longer needed - server extracts from JPG
                    float face_embedding[1] = {0};
                    int embedding_size = 0;

                    // 1. Check if we have any enrolled faces
                    if (recognizer->get_enrolled_id_num() == 0)
                    {
                        // First time detection - auto enroll
                        recognizer->enroll_id((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_results.front().keypoint, "", true);
                        stored_face_id = recognizer->get_enrolled_ids().back().id;
                        faces_enrolled++;
                        ESP_LOGI(TAG, "🎉 FIRST FACE ENROLLED: ID %d", stored_face_id);
                        frame_show_state = SHOW_STATE_ENROLL;
                        
                        // Convert current frame to JPG for server-side processing
                        uint8_t *jpg_buf = NULL;
                        size_t jpg_len = 0;
                        if (frame2jpg(frame, 60, &jpg_buf, &jpg_len)) {
                            ESP_LOGI(TAG, "📸 Frame converted to JPG (%d bytes)", jpg_len);
                            csv_logger_log_face(stored_face_id, face_embedding, embedding_size, csv_gps, jpg_buf, jpg_len);
                            free(jpg_buf);
                        } else {
                            ESP_LOGE(TAG, "❌ JPG conversion failed");
                            csv_logger_log_face(stored_face_id, face_embedding, embedding_size, csv_gps, NULL, 0);
                        }
                        
                        s_last_detection_us = esp_timer_get_time();
                        csv_uploader_trigger_now();
                    }
                    else
                    {
                        // Face recognition for subsequent detections
                        recognize_result = recognizer->recognize((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_results.front().keypoint);
                        
                        ESP_LOGI(TAG, "RECOGNIZE: Similarity: %f, Match ID: %d, Threshold: %f", 
                                 recognize_result.similarity, recognize_result.id, SIMILARITY_THRESHOLD);
                        
                        // Check if similarity is above threshold
                        if (recognize_result.id > 0 && !isnan(recognize_result.similarity) && recognize_result.similarity >= SIMILARITY_THRESHOLD) {
                            ESP_LOGI(TAG, "✅ FACE MATCHED - ID: %d, Similarity: %.3f", recognize_result.id, recognize_result.similarity);
                            frame_show_state = SHOW_STATE_RECOGNIZE;
                        } else {
                            // Different person detected (low similarity)
                            ESP_LOGW(TAG, "🆕 NEW PERSON DETECTED (Similarity too low)");
                            
                            int64_t now_us = esp_timer_get_time();
                            // Implementation of 30-second throttle
                            if ((now_us - s_last_detection_us) >= (30 * 1000 * 1000)) {
                                // Clear old and enroll new
                                while (recognizer->get_enrolled_id_num() > 0) recognizer->delete_id(true);
                                
                                recognizer->enroll_id((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_results.front().keypoint, "", true);
                                stored_face_id = recognizer->get_enrolled_ids().back().id;
                                faces_enrolled++;
                                ESP_LOGI(TAG, "🔄 REPLACED FACE: ID %d", stored_face_id);
                                frame_show_state = SHOW_STATE_ENROLL;

                                // Send new face to server
                                uint8_t *jpg_buf = NULL;
                                size_t jpg_len = 0;
                                if (frame2jpg(frame, 60, &jpg_buf, &jpg_len)) {
                                    csv_logger_log_face(stored_face_id, face_embedding, embedding_size, csv_gps, jpg_buf, jpg_len);
                                    free(jpg_buf);
                                } else {
                                    csv_logger_log_face(stored_face_id, face_embedding, embedding_size, csv_gps, NULL, 0);
                                }
                                
                                s_last_detection_us = now_us;
                                csv_uploader_trigger_now();
                            } else {
                                ESP_LOGW(TAG, "Throttling new face detection (cooldown active)");
                            }
                        }
                    }
                }

                if (frame_show_state != SHOW_STATE_IDLE)
                {
                    static int frame_count = 0;
                    if (++frame_count > FRAME_DELAY_NUM)
                    {
                        frame_count = 0;
                        frame_show_state = SHOW_STATE_IDLE;
                    }
                }

                if (xQueueFrameO)
                {
                    if (xQueueSend(xQueueFrameO, &frame, pdMS_TO_TICKS(10)) != pdTRUE)
                    {
                        if (gReturnFB) esp_camera_fb_return(frame);
                        else free(frame);
                    }
                }
                else if (gReturnFB)
                {
                    esp_camera_fb_return(frame);
                }
                else
                {
                    free(frame);
                }

                if (xQueueResult && is_detected)
                {
                    xQueueSend(xQueueResult, &recognize_result, portMAX_DELAY);
                }
            }
        }
    }
}

static void task_event_handler(void *arg)
{
    recognizer_state_t _gEvent;
    while (true)
    {
        xQueueReceive(xQueueEvent, &(_gEvent), portMAX_DELAY);
        xSemaphoreTake(xMutex, portMAX_DELAY);
        gEvent = _gEvent;
        xSemaphoreGive(xMutex);
    }
}

void register_human_face_recognition(const QueueHandle_t frame_i,
                                     const QueueHandle_t event,
                                     const QueueHandle_t result,
                                     const QueueHandle_t frame_o,
                                     const bool camera_fb_return)
{
    xQueueFrameI = frame_i;
    xQueueFrameO = frame_o;
    xQueueEvent = event;
    xQueueResult = result;
    gReturnFB = camera_fb_return;
    xMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(task_process_handler, TAG, 8 * 1024, NULL, 5, NULL, 0);
    if (xQueueEvent)
        xTaskCreatePinnedToCore(task_event_handler, TAG, 8 * 1024, NULL, 5, NULL, 1);
}
