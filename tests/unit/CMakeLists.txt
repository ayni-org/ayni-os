include(GoogleTest)

add_executable(ayni_unit_tests
    batch_codec_test.cpp
    completion_pool_test.cpp
    event_queue_test.cpp
    kernel_api_test.cpp
    kernel_lifecycle_test.cpp
    native_host_test.cpp
    native_node_test.cpp
    prefix_scan_test.cpp
    sim_host_test.cpp)
# ayni_core_internal gives white-box tests the Core-internal include root.
target_link_libraries(ayni_unit_tests PRIVATE
    ayni_host_native ayni_host_sim ayni_runtime_native_node ayni_core ayni_core_internal
    ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_unit_tests)
