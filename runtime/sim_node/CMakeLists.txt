add_library(ayni_runtime_sim_node STATIC sim_node.cpp)
target_include_directories(ayni_runtime_sim_node PUBLIC include)
target_link_libraries(ayni_runtime_sim_node PUBLIC ayni_api ayni_runtime_common ayni_host_sim
    PRIVATE ayni_core ayni_runtime_common ayni_warnings)

# Compile the closed normal profile once; native runners never parse JSON in Core.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(normal_fixture_source "${CMAKE_CURRENT_BINARY_DIR}/generated/normal_fixture_cases.cpp")
add_custom_target(ayni_generate_normal_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/generate_normal_fixtures.py"
        --root "${AYNI_ROOT}" --output "${normal_fixture_source}"
    BYPRODUCTS "${normal_fixture_source}"
    WORKING_DIRECTORY "${AYNI_ROOT}" VERBATIM)
add_library(ayni_fixture_model STATIC fixture_model.cpp)
target_link_libraries(ayni_fixture_model PUBLIC ayni_api ayni_runtime_common ayni_provider_mock PRIVATE ayni_warnings)
add_library(ayni_sim_normal_fixtures STATIC normal_fixtures.cpp
    normal_fixture_profile.cpp multinode_transport.cpp multinode_normal.cpp
    "${normal_fixture_source}")
add_dependencies(ayni_sim_normal_fixtures ayni_generate_normal_fixtures)
target_include_directories(ayni_sim_normal_fixtures PUBLIC include)
target_compile_definitions(ayni_sim_normal_fixtures PRIVATE
    AYNI_FIXTURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_FIXTURE_SANITIZER="${AYNI_SANITIZE}")
target_link_libraries(ayni_sim_normal_fixtures PUBLIC ayni_api ayni_normal_fixture_policy PRIVATE ayni_runtime_sim_node
    ayni_provider_mock ayni_fixture_model ayni_assistant_application ayni_warnings)

set(experiment_source "${CMAKE_CURRENT_BINARY_DIR}/generated/experiment_cases.cpp")
add_custom_target(ayni_generate_experiment_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/generate_experiment_fixtures.py"
        --root "${AYNI_ROOT}" --output "${experiment_source}"
    BYPRODUCTS "${experiment_source}"
    WORKING_DIRECTORY "${AYNI_ROOT}" VERBATIM)
add_library(ayni_sim_experiments STATIC experiment_evidence.cpp experiment_service.cpp experiment_world.cpp experiment_execution.cpp experiment_shared.cpp experiment_shared_memory.cpp experiment_shared_limits.cpp experiment_shared_resources.cpp experiment_shared_invalidation.cpp experiment_shared_presentation.cpp experiment_shared_ingress.cpp
    experiment_runner.cpp "${experiment_source}")
add_dependencies(ayni_sim_experiments ayni_generate_experiment_fixtures)
target_include_directories(ayni_sim_experiments PUBLIC include PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ayni_sim_experiments PRIVATE
    AYNI_FIXTURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_FIXTURE_SANITIZER="${AYNI_SANITIZE}")
target_link_libraries(ayni_sim_experiments PUBLIC ayni_runtime_sim_node PRIVATE ayni_provider_mock_execution ayni_fixture_model ayni_normal_fixture_policy ayni_experiment_policy ayni_assistant_application ayni_strategies ayni_warnings)
# White-box fault injection reuses Core codecs only in the tests/ source. This
# optional test dependency adds no Core header/type to production runtime APIs.
if(AYNI_BUILD_TESTS)
    target_sources(ayni_sim_experiments PRIVATE "${AYNI_ROOT}/tests/experiments/record_cut.cpp" "${AYNI_ROOT}/tests/experiments/completion_slots.cpp")
    target_include_directories(ayni_sim_experiments PRIVATE "${AYNI_ROOT}/core")
else()
    target_sources(ayni_sim_experiments PRIVATE experiment_no_record_probe.cpp)
endif()
