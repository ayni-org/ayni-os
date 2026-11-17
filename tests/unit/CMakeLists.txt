add_executable(ayni_unit_tests
    commit_group_test.cpp
    binding_instances_test.cpp
    kernel_shell_call_test.cpp
    kernel_business_call_test.cpp
    workspace_test.cpp
    business_arguments_test.cpp
    business_values_test.cpp
    business_schema_test.cpp
    business_projection_test.cpp
    shell_process_test.cpp
    calendar_test.cpp
    kernel_calendar_test.cpp
    kernel_pause_group_test.cpp
    kernel_memory_test.cpp
    kernel_context_test.cpp
    memory_table_test.cpp
    memory_decision_test.cpp
    provenance_test.cpp
    observation_test.cpp
    observation_ingress_test.cpp
    kernel_subscription_test.cpp
    subscription_facts_test.cpp
    mock_media_test.cpp
    scene_test.cpp
    managed_execution_test.cpp
    execution_facts_test.cpp
    control_gate_test.cpp
    mock_provider_test.cpp
    mock_execution_test.cpp
    mock_tools_test.cpp
    action_record_test.cpp
    call_input_test.cpp
    call_table_test.cpp
    call_contract_test.cpp
    tool_calls_test.cpp
    kernel_tool_call_test.cpp
    resource_admission_test.cpp
    kernel_layout_test.cpp
    kernel_resource_test.cpp
    kernel_provider_test.cpp
    kernel_call_test.cpp
    agent_table_test.cpp
    authority_cleanup_test.cpp
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
    kernel_goal_authority_test.cpp
    command_table_test.cpp
    intent_policy_matrix_test.cpp
    work_authority_result_test.cpp
    completion_pool_test.cpp
    provider_mailbox_test.cpp
    executor_mailbox_test.cpp
    event_queue_test.cpp
    field_values_test.cpp
    goal_table_test.cpp
    goal_authority_test.cpp
    goal_authority_persistence_test.cpp
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
    shared_storage_test.cpp
    esp_space_ledger_test.cpp
    file_value_test.cpp
    native_host_test.cpp
    native_node_test.cpp
    owner_binding_records_test.cpp
    owner_binding_snapshot_test.cpp
    persistent_store_test.cpp
    prefix_scan_test.cpp
    principal_registry_test.cpp
    capability_registry_test.cpp
    scope_test.cpp
    kernel_capability_test.cpp
    sim_host_test.cpp
    storage_budget_test.cpp
    strategy_registry_test.cpp
    task_table_test.cpp
    task_call_test.cpp
    task_runtime_limits_test.cpp
    task_authority_test.cpp
    trigger_schedule_test.cpp
    work_records_test.cpp
    work_snapshot_test.cpp
    work_restore_test.cpp)
# ayni_core_internal gives white-box tests the Core-internal include root.
target_link_libraries(ayni_unit_tests PRIVATE
    ayni_core ayni_core_internal ayni_host_native ayni_host_storage ayni_host_sim ayni_runtime_conversation ayni_runtime_native_node ayni_provider_mock ayni_provider_mock_execution ayni_provider_shell ayni_control_gate
    ayni_provider_software ayni_warnings GTest::gtest_main)
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
gtest_discover_tests(ayni_unit_tests DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

# Separate protocol executable keeps network fixture dependencies out of Core tests.
add_executable(ayni_openai_provider_tests openai_provider_test.cpp)
target_link_libraries(ayni_openai_provider_tests PRIVATE ayni_core ayni_core_internal
    ayni_openai_protocol_fixture ayni_runtime_common
    ayni_host_sim ayni_provider_mock ayni_provider_openai_compat yyjson ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_openai_provider_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
add_executable(ayni_model_protocol_tests model_protocol_test.cpp)
target_link_libraries(ayni_model_protocol_tests PRIVATE ayni_model_protocol yyjson
    ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_model_protocol_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

# No CTest registration: this executable sends billed requests only when invoked
# manually with --live and runtime environment configuration (ADR-0055).
add_executable(ayni_openai_smoke EXCLUDE_FROM_ALL ../manual/openai_smoke.cpp)
target_link_libraries(ayni_openai_smoke PRIVATE ayni_core ayni_core_internal ayni_runtime_common
    ayni_host_sim ayni_provider_mock ayni_provider_openai_compat ayni_warnings GTest::gtest)

add_executable(ayni_strategies_tests shared_model_loop_test.cpp strategies_test.cpp conversation_test.cpp history_loading_test.cpp harness_context_test.cpp managed_operations_test.cpp)
target_link_libraries(ayni_strategies_tests PRIVATE ayni_core ayni_core_internal ayni_runtime_common
    ayni_host_sim ayni_strategies ayni_model_protocol yyjson ayni_provider_software ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_strategies_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

add_executable(ayni_native_assistant_tests native_assistant_test.cpp kernel_query_test.cpp sqlite_values_test.cpp)
target_link_libraries(ayni_native_assistant_tests PRIVATE ayni_core ayni_core_internal
    ayni_openai_protocol_fixture
    ayni_host_sim ayni_runtime_assistant ayni_runtime_conversation ayni_runtime_sim_node ayni_runtime_demo
    ayni_warnings GTest::gtest_main SQLite::SQLite3)
gtest_discover_tests(ayni_native_assistant_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

add_executable(ayni_assistant_application_tests assistant_application_test.cpp)
target_link_libraries(ayni_assistant_application_tests PRIVATE ayni_core ayni_runtime_demo
    ayni_runtime_sim_node ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_assistant_application_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

add_executable(ayni_cpp_sdk_tests cpp_sdk_test.cpp runtime_examples_test.cpp json_writer_test.cpp)
# Keep GoogleTest factories compatible with its RTTI-enabled sanitized library.
# The standalone SDK smoke and examples enforce no-exceptions/no-RTTI clients.
target_link_libraries(ayni_cpp_sdk_tests PRIVATE ayni_core ayni_host_sim ayni_runtime_common ayni_runtime_examples
    ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_cpp_sdk_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

# Public SDK integration: the interactive consumer shares Session/Profile with ESP.
add_executable(ayni_conversation_session_tests conversation_session_test.cpp)
target_link_libraries(ayni_conversation_session_tests PRIVATE ayni_runtime_portable
    ayni_host_native ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_conversation_session_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

# Sim Core + production file values keep publication progress deterministic across Hosts.
add_executable(ayni_conversation_progress_tests conversation_progress_test.cpp)
target_link_libraries(ayni_conversation_progress_tests PRIVATE ayni_runtime_portable
    ayni_host_sim ayni_host_native ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_conversation_progress_tests
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
