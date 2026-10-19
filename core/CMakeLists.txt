# Portable Core. Self-contained so that both the native project and the
# ESP-IDF runtime component can add it with add_subdirectory (ADR-0011).
# Modules get a target only once they have code; core/README.md lists all nine.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AyniLayers.cmake")

ayni_add_core_module(kernel
    SOURCES
        kernel/completion/completion_pool.cpp
        kernel/ingress/event_queue.cpp
        kernel/kernel_api.cpp
        kernel/kernel_layout.cpp)

# What consumers link: the top module pulls in every module below it.
add_library(ayni_core INTERFACE)
target_link_libraries(ayni_core INTERFACE ayni_core_kernel)
