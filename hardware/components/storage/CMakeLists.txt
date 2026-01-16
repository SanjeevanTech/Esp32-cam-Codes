idf_component_register(SRCS "csv_logger.c"
                             "csv_uploader.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json fatfs sdmmc esp_wifi esp-tls)

# Optimize storage component for size to save IRAM
component_compile_options(-Os -ffunction-sections -fdata-sections -Wno-format-truncation)

# Move large functions to flash memory instead of IRAM
set_source_files_properties(csv_logger.c PROPERTIES COMPILE_FLAGS "-Os -fno-inline-functions -Wno-format-truncation")
set_source_files_properties(csv_uploader.c PROPERTIES COMPILE_FLAGS "-Os -fno-inline-functions")