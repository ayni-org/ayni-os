include(GoogleTest)

add_executable(ayni_unit_tests
    agent_table_test.cpp
    batch_codec_test.cpp
    completion_pool_test.cpp
    event_queue_test.cpp
    goal_table_test.cpp
    goal_type_registry_test.cpp
    kernel_api_test.cpp
    kernel_id_test.cpp
    kernel_lifecycle_test.cpp
    kernel_work_test.cpp
    native_host_test.cpp
    native_node_test.cpp
    prefix_scan_test.cpp
    principal_registry_test.cpp
    sim_host_test.cpp)
# ayni_core_internal gives white-box tests the Core-internal include root.
target_link_libraries(ayni_unit_tests PRIVATE
    ayni_core ayni_core_internal ayni_host_native ayni_host_sim ayni_runtime_native_node
    ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_unit_tests)
