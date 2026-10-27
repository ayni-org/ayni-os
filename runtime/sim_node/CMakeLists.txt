add_library(ayni_runtime_sim_node STATIC sim_node.cpp)
target_include_directories(ayni_runtime_sim_node PUBLIC include)
target_link_libraries(ayni_runtime_sim_node PUBLIC ayni_api ayni_runtime_common
    PRIVATE ayni_core ayni_host_sim ayni_runtime_common ayni_warnings)

# Compile the closed normal profile once; native runners never parse JSON in Core.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(normal_fixture_source "${CMAKE_CURRENT_BINARY_DIR}/generated/normal_fixture_cases.cpp")
add_custom_target(ayni_generate_normal_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/generate_normal_fixtures.py"
        --root "${AYNI_ROOT}" --output "${normal_fixture_source}"
    BYPRODUCTS "${normal_fixture_source}"
    WORKING_DIRECTORY "${AYNI_ROOT}" VERBATIM)
add_library(ayni_sim_normal_fixtures STATIC normal_fixtures.cpp
    normal_fixture_profile.cpp "${normal_fixture_source}")
add_dependencies(ayni_sim_normal_fixtures ayni_generate_normal_fixtures)
target_include_directories(ayni_sim_normal_fixtures PUBLIC include)
target_compile_definitions(ayni_sim_normal_fixtures PRIVATE
    AYNI_FIXTURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_FIXTURE_SANITIZER="${AYNI_SANITIZE}")
target_link_libraries(ayni_sim_normal_fixtures PUBLIC ayni_api ayni_normal_fixture_policy PRIVATE ayni_runtime_sim_node
    ayni_provider_mock ayni_assistant_application ayni_warnings)
