if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Ayni Native Host currently supports Linux; select a platform Host explicitly")
endif()
find_package(Threads REQUIRED)

add_library(ayni_host_native STATIC native_host.cpp posix_files.cpp)
target_include_directories(ayni_host_native PUBLIC include)
# Links Core for ayni_completion_post, which the I/O thread calls to complete
# storage requests.
target_link_libraries(ayni_host_native
    PUBLIC ayni_api
    PRIVATE ayni_core ayni_host_storage Threads::Threads ayni_warnings)
