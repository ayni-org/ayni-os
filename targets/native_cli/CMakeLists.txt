add_executable(ayni main.cpp config.cpp commands.cpp format.cpp)
target_link_libraries(ayni PRIVATE ayni_runtime_assistant ayni_warnings)
