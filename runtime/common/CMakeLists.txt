# Header-only pieces every composition root shares (ADR-0053). The ESP-IDF
# component adds the same include directory.
add_library(ayni_runtime_common INTERFACE)
target_include_directories(ayni_runtime_common INTERFACE include)
target_link_libraries(ayni_runtime_common INTERFACE ayni_api)
