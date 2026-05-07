cmake_minimum_required(VERSION 3.16)

# Force C++17 to avoid C++20/23 compatibility issues in Matter SDK
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable warnings-as-errors globally
add_compile_options(-Wno-error -DCHIP_HAVE_CONFIG_H=1)

# Define Matter SDK path for components
set(MATTER_SDK_PATH "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip")

# Correctly define Matter component directories
set(EXTRA_COMPONENT_DIRS 
    "$ENV{ESP_MATTER_PATH}/components" 
    "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/config/esp32/components/chip"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_delay_switch)

