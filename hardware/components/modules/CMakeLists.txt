# HTML files removed to save memory - using React frontend instead
# set(embed_files "web/www/index_ov2640.html.gz"
#                 "web/www/power_minimal.html.gz")
set(embed_files)

set(src_dirs    
                ai
                camera
                web
                gps
                power)

set(include_dirs    
                    ai
                    camera
                    web
                    gps
                    power)

set(requires    esp32-camera
                esp-dl
                esp_http_server
                esp_http_client
                nvs_flash
                mdns
                driver
                json
                storage
                esp-tls)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires} EMBED_FILES ${embed_files})

# Optimize for size to reduce IRAM usage
component_compile_options(-Os -ffunction-sections -fdata-sections -fno-inline-functions)

# Optimize AI modules for size instead of speed to save IRAM
set_source_files_properties(ai/who_human_face_recognition.cpp PROPERTIES COMPILE_FLAGS "-Os -fno-inline-functions -fno-inline-small-functions")

# Optimize web components for size
set_source_files_properties(web/app_httpd.cpp PROPERTIES COMPILE_FLAGS "-Os -fno-inline-functions")
