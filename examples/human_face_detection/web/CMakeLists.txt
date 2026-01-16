# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ../../../hardware/components)

# 🚀 AGGRESSIVE IRAM OPTIMIZATION
set(CMAKE_BUILD_TYPE Release)
add_compile_options(-Os -ffunction-sections -fdata-sections -fno-inline-functions -fno-inline-small-functions)
add_compile_options(-fdiagnostics-color=always)

# Reduce debug info to save space
add_compile_options(-g1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(human_face_detection_web)
