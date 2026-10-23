include(GoogleTest)

add_executable(ayni_unit_tests
    agent_table_test.cpp
    batch_codec_test.cpp
    command_revoke_test.cpp
    pending_revoke_test.cpp
    kernel_revoke_test.cpp
    grant_revoke_test.cpp
    command_canonical_test.cpp
    command_records_test.cpp
    grant_replay_test.cpp
    grant_records_test.cpp
    grant_table_test.cpp
    kernel_grant_test.cpp
    command_table_test.cpp
    completion_pool_test.cpp
    event_queue_test.cpp
    field_values_test.cpp
    goal_table_test.cpp
    goal_authority_test.cpp
    goal_type_registry_test.cpp
    host_storage_contract_test.cpp
    journal_ring_test.cpp
    kernel_api_test.cpp
    kernel_checkpoint_test.cpp
    kernel_event_test.cpp
    kernel_expiry_test.cpp
    kernel_fence_test.cpp
    kernel_id_test.cpp
    kernel_lifecycle_test.cpp
    kernel_owner_binding_test.cpp
    kernel_recovery_test.cpp
    kernel_storage_recovery_test.cpp
    kernel_snapshot_fault_test.cpp
    kernel_storage_admission_test.cpp
    kernel_strategy_test.cpp
    kernel_task_test.cpp
    kernel_trigger_test.cpp
    kernel_work_test.cpp
    native_host_test.cpp
    native_node_test.cpp
    owner_binding_records_test.cpp
    owner_binding_snapshot_test.cpp
    prefix_scan_test.cpp
    principal_registry_test.cpp
    capability_registry_test.cpp
    scope_test.cpp
    kernel_capability_test.cpp
    sim_host_test.cpp
    storage_budget_test.cpp
    strategy_registry_test.cpp
    task_table_test.cpp
    trigger_schedule_test.cpp
    work_records_test.cpp
    work_snapshot_test.cpp
    work_restore_test.cpp)
# ayni_core_internal gives white-box tests the Core-internal include root.
target_link_libraries(ayni_unit_tests PRIVATE
    ayni_core ayni_core_internal ayni_host_native ayni_host_sim ayni_runtime_native_node
    ayni_warnings GTest::gtest_main)
# Every test file includes gtest.h, which takes about a second to parse, so the
# headers most test files share are parsed once per build. Test files still
# include what they use; the weekly clang-tidy run builds without this.
target_precompile_headers(ayni_unit_tests PRIVATE
    <gtest/gtest.h> <array> <cstddef> <cstdint> <cstring> <limits> <string> <vector>)
# Clang otherwise stamps the precompiled header with its build time, and
# ccache then misses every test file in a fresh build directory.
target_compile_options(ayni_unit_tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:SHELL:-Xclang -fno-pch-timestamp>")
# Release still builds Core at -O3. Building the test code that drives it at
# -O3 made the Release build about 40% slower and checked nothing more.
target_compile_options(ayni_unit_tests PRIVATE $<$<CONFIG:Release>:-O1>)
gtest_discover_tests(ayni_unit_tests)
