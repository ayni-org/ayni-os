# Static worker reclamation is proven against IDF6's default legacy dual-core
# kernel. The optional upstream SMP kernel has different suspended-state semantics.
if(CONFIG_FREERTOS_SMP)
    message(FATAL_ERROR "Ayni ESP Host requires CONFIG_FREERTOS_SMP=n; upstream SMP join is not supported")
endif()
# ESP-IDF component. ESP-IDF names components after their directory, so this
# one is "esp_idf"; runtime/esp_idf_node requires it under that name.
idf_component_register(
    SRCS "esp_idf_host.cpp" "littlefs_files.cpp" "storage_host.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_hw_support esp_psram heap spi_flash log esp_partition joltwallet__littlefs)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/AyniSDK.cmake")
ayni_add_sdk(COMPONENTS host_storage)
target_link_libraries(${COMPONENT_LIB} PUBLIC ayni_api ayni_host_storage PRIVATE ayni_platform_warnings)

# Vendor raw headers contain inline conversions outside Ayni's warnings policy.
# Keep strict diagnostics for our adapter while treating the dependency as system headers.
idf_component_get_property(littlefs_dir joltwallet__littlefs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE "${littlefs_dir}/include")
