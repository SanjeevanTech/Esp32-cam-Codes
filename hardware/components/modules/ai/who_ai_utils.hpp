#pragma once

#include <list>
#include "dl_detect_define.hpp"
#include "esp_camera.h"

// Removed draw_detection_result function declarations - visual overlays not needed

/**
 * @brief Print detection result in terminal
 * 
 * @param results detection results
 */
void print_detection_result(std::list<dl::detect::result_t> &results);

/**
 * @brief Decode fb , 
 *        - if fb->format == PIXFORMAT_RGB565, then return fb->buf
 *        - else, then return a new memory with RGB888, don't forget to free it
 * 
 * @param fb 
 */
void *app_camera_decode(camera_fb_t *fb);
