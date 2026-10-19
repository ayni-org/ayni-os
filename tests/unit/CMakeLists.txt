include(GoogleTest)

add_executable(ayni_unit_tests
    kernel_api_test.cpp
    native_host_test.cpp
    native_node_test.cpp)
target_link_libraries(ayni_unit_tests PRIVATE
    ayni_core ayni_host_native ayni_runtime_native_node ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_unit_tests)
