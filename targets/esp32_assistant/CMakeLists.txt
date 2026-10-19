# ESP32-S3 firmware (ADR-0010): a thin entry point over runtime/esp_idf_node.
# Build from this directory with the pinned ESP-IDF (docs/development.md).
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../hosts/esp_idf"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/esp_idf_node")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ayni_esp32_assistant)
