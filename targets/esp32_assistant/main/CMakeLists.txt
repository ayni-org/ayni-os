idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES esp_idf_node log)

target_link_libraries(${COMPONENT_LIB} PRIVATE ayni_platform_warnings)
