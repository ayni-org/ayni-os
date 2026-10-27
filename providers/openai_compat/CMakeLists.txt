include("${AYNI_ROOT}/cmake/AyniYyjson.cmake")
find_package(CURL 7.85 REQUIRED)
find_package(Threads REQUIRED)
add_library(ayni_openai_protocol_fixture INTERFACE)
target_include_directories(ayni_openai_protocol_fixture INTERFACE test_support/include)
add_library(ayni_provider_openai_compat STATIC provider.cpp protocol.cpp)
target_include_directories(ayni_provider_openai_compat PUBLIC include)
target_link_libraries(ayni_provider_openai_compat PUBLIC ayni_api
    PRIVATE ayni_core ayni_warnings CURL::libcurl yyjson Threads::Threads)
