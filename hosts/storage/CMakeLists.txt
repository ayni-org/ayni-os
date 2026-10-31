add_library(ayni_host_storage STATIC file_log.cpp request.cpp file_value.cpp)
target_include_directories(ayni_host_storage PUBLIC include)
target_link_libraries(ayni_host_storage PUBLIC ayni_api PRIVATE ayni_core ayni_warnings)
