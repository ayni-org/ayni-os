# ESP-IDF component "esp_idf_node": the composition root for ESP-IDF targets
# (ADR-0010). It brings in the same Core as the native build.
idf_component_register(
    SRCS "esp_idf_node.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_idf heap log)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../core" "${CMAKE_CURRENT_BINARY_DIR}/ayni_core")
target_link_libraries(${COMPONENT_LIB} PUBLIC ayni_api PRIVATE ayni_core ayni_platform_warnings)
