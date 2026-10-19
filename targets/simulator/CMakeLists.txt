add_executable(ayni-sim main.cpp)
target_link_libraries(ayni-sim PRIVATE ayni_runtime_native_node ayni_warnings)
