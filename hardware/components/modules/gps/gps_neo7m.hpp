#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "gps_types.h"

// GPS configuration
typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
} gps_config_t;

// NMEA sentence types
typedef enum {
    NMEA_GPGGA,  // Global Positioning System Fix Data
    NMEA_GPRMC,  // Recommended Minimum Course
    NMEA_GPGSV,  // Satellites in View
    NMEA_UNKNOWN
} nmea_sentence_type_t;

// Function declarations
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gps_init(gps_config_t *config);
esp_err_t gps_start(void);
esp_err_t gps_stop(void);
gps_data_t gps_get_current_data(void);
bool gps_is_valid(void);
void gps_task(void *pvParameters);

#ifdef __cplusplus
}
#endif