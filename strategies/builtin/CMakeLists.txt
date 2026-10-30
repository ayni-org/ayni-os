add_library(ayni_strategies STATIC support.cpp conversation.cpp rules.cpp tool_loop.cpp harness_tools.cpp harness_operations.cpp harness_summary.cpp)
target_include_directories(ayni_strategies PUBLIC include)
target_link_libraries(ayni_strategies PUBLIC ayni_api PRIVATE ayni_core ayni_core_options)
