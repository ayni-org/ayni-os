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
add_library(ayni_sim_normal_fixtures STATIC normal_fixtures.cpp
    normal_fixture_profile.cpp multinode_transport.cpp multinode_normal.cpp
    "${normal_fixture_source}")
add_dependencies(ayni_sim_normal_fixtures ayni_generate_normal_fixtures)
target_include_directories(ayni_sim_normal_fixtures PUBLIC include)
target_compile_definitions(ayni_sim_normal_fixtures PRIVATE
    AYNI_FIXTURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_FIXTURE_SANITIZER="${AYNI_SANITIZE}")
target_link_libraries(ayni_sim_normal_fixtures PUBLIC ayni_api ayni_normal_fixture_policy PRIVATE ayni_runtime_sim_node
    ayni_provider_mock ayni_assistant_application ayni_warnings)

set(experiment_source "${CMAKE_CURRENT_BINARY_DIR}/generated/experiment_cases.cpp")
add_custom_target(ayni_generate_experiment_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/generate_experiment_fixtures.py"
        --root "${AYNI_ROOT}" --output "${experiment_source}"
    BYPRODUCTS "${experiment_source}"
    WORKING_DIRECTORY "${AYNI_ROOT}" VERBATIM)
add_library(ayni_sim_experiments STATIC experiment_evidence.cpp experiment_service.cpp experiment_world.cpp
    experiment_runner.cpp "${experiment_source}")
add_dependencies(ayni_sim_experiments ayni_generate_experiment_fixtures)
target_include_directories(ayni_sim_experiments PUBLIC include PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ayni_sim_experiments PRIVATE
    AYNI_FIXTURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_FIXTURE_SANITIZER="${AYNI_SANITIZE}")
target_link_libraries(ayni_sim_experiments PUBLIC ayni_runtime_sim_node PRIVATE ayni_warnings)
