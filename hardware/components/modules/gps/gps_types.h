#pragma once

#include <stdbool.h>

// GPS data structure - shared between GPS and face tracking modules
typedef struct {
    float latitude;
    float longitude;
    float altitude;
    int satellites;
    bool valid;
    char timestamp[32];
} gps_data_t;