find_package(Threads REQUIRED)

add_library(ayni_host_native STATIC native_host.cpp file_log.cpp)
target_include_directories(ayni_host_native PUBLIC include)
# Links Core for ayni_completion_post, which the I/O thread calls to complete
# storage requests.
target_link_libraries(ayni_host_native
    PUBLIC ayni_api
    PRIVATE ayni_core Threads::Threads ayni_warnings)
