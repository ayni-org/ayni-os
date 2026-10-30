# Header-only pieces every composition root shares (ADR-0053). The ESP-IDF
# component adds the same include directory.
add_library(ayni_runtime_common INTERFACE)
target_include_directories(ayni_runtime_common INTERFACE include)
target_link_libraries(ayni_runtime_common INTERFACE ayni_api)

# Native/Sim demo wiring is a separate target: ESP-IDF still uses only the
# header-only default configuration above and never links curl or Mock profiles.
add_library(ayni_runtime_demo STATIC demo_profile.cpp)
target_include_directories(ayni_runtime_demo PUBLIC include)
target_link_libraries(ayni_runtime_demo PUBLIC ayni_api ayni_assistant_application ayni_provider_presentation ayni_strategies
    PRIVATE ayni_core ayni_provider_mock ayni_provider_openai_compat ayni_provider_shell ayni_warnings)
