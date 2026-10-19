# Portable Core. Self-contained so that both the native project and the
# ESP-IDF runtime component can add it with add_subdirectory (ADR-0011).
# Modules get a target only once they have code; core/README.md lists all nine.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AyniLayers.cmake")

ayni_add_core_module(foundation
    SOURCES
        foundation/crc32.cpp)

ayni_add_core_module(durability
    SOURCES
        durability/batch/batch_codec.cpp
        durability/batch/prefix_scan.cpp
    DEPENDS foundation)

ayni_add_core_module(catalog
    SOURCES
        catalog/catalog_id.cpp
        catalog/goal_type/goal_type_registry.cpp
    DEPENDS foundation)

ayni_add_core_module(authority
    SOURCES
        authority/principal/principal_registry.cpp
    DEPENDS foundation)

ayni_add_core_module(work
    SOURCES
        work/agent/agent_table.cpp
        work/goal/goal_table.cpp
    DEPENDS foundation)

ayni_add_core_module(kernel
    SOURCES
        kernel/boot/boot_sequence.cpp
        kernel/completion/completion_pool.cpp
        kernel/ingress/event_queue.cpp
        kernel/intent/goal_intents.cpp
        kernel/intent/intent.cpp
        kernel/intent/work_model.cpp
        kernel/kernel_api.cpp
        kernel/kernel_layout.cpp
        kernel/kernel_lifecycle.cpp
        kernel/work_api.cpp
    DEPENDS work authority catalog durability foundation)

# What consumers link: the top module pulls in every module below it.
add_library(ayni_core INTERFACE)
target_link_libraries(ayni_core INTERFACE ayni_core_kernel)
