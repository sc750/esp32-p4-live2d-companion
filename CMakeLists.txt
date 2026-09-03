# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

# ESP32-P4 目标芯片
set(IDF_TARGET "esp32p4")

# 注册自定义组件目录
set(EXTRA_COMPONENT_DIRS "user")

# 防止 Windows 路径过长
set(CMAKE_OBJECT_PATH_MAX 512)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(project1)
