# Concrete services live only in this composition root; example clients use public APIs.
add_library(ayni_runtime_examples STATIC examples.cpp)
target_include_directories(ayni_runtime_examples PUBLIC include)
target_link_libraries(ayni_runtime_examples PUBLIC ayni_api PRIVATE ayni_core ayni_host_sim ayni_host_native
    ayni_runtime_demo ayni_software_workflow ayni_provider_software ayni_provider_openai_compat ayni_openai_protocol_fixture ayni_warnings)
