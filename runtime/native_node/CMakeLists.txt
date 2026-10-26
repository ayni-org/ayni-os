add_library(ayni_runtime_native_node STATIC native_node.cpp)
target_include_directories(ayni_runtime_native_node PUBLIC include)
target_link_libraries(ayni_runtime_native_node
    PUBLIC ayni_api
    PRIVATE ayni_core ayni_host_native ayni_runtime_common ayni_warnings)
