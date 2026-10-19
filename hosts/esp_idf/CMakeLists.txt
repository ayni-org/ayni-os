# ESP-IDF component. ESP-IDF names components after their directory, so this
# one is "esp_idf"; runtime/esp_idf_node requires it under that name.
idf_component_register(
    SRCS "esp_idf_host.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_hw_support esp_psram heap spi_flash log)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/AyniCommon.cmake")
target_link_libraries(${COMPONENT_LIB} PUBLIC ayni_api PRIVATE ayni_platform_warnings)
