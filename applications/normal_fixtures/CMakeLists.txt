# Trusted fixture-only workflows. Runtime selects concrete services; policies decide steps.
add_library(ayni_normal_fixture_policy STATIC policy.cpp)
target_include_directories(ayni_normal_fixture_policy PUBLIC include)
target_link_libraries(ayni_normal_fixture_policy PUBLIC ayni_api
    PRIVATE ayni_core_options)
