include(${CMAKE_SOURCE_DIR}/cmake/AyniYyjson.cmake)
add_library(ayni_provider_mock STATIC mock.cpp tool_plan.cpp media.cpp subscription.cpp)
target_include_directories(ayni_provider_mock PUBLIC include)
target_link_libraries(ayni_provider_mock PUBLIC ayni_api PRIVATE ayni_core ayni_core_options yyjson)
