find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_test(NAME SdkImport.isolated_consumer
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tests/portability/sdk_import_test.py"
        --source "${AYNI_ROOT}" --optional --download-dir "${AYNI_DOWNLOAD_DIR}"
        --sanitize "${AYNI_SANITIZE}" --generator "${CMAKE_GENERATOR}")
set_tests_properties(SdkImport.isolated_consumer PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
foreach(kind IN ITEMS host-storage native-host)
    string(REPLACE "-" "_" probe "${kind}")
    add_test(NAME SdkImport.${probe}
        COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tests/portability/sdk_import_test.py"
            --source "${AYNI_ROOT}" --${kind} --download-dir "${AYNI_DOWNLOAD_DIR}"
            --sanitize "${AYNI_SANITIZE}" --generator "${CMAKE_GENERATOR}")
    set_tests_properties(SdkImport.${probe} PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
endforeach()
add_test(NAME NativeCli.scripted_sessions COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/native_cli_test.py $<TARGET_FILE:ayni>)
set_tests_properties(NativeCli.scripted_sessions PROPERTIES TIMEOUT 90)

add_subdirectory(esp32_wifi_chat)

add_executable(ayni_normal_fixture_tests normal_fixture_test.cpp)
target_link_libraries(ayni_normal_fixture_tests PRIVATE ayni_sim_normal_fixtures
    ayni_warnings GTest::gtest_main)
target_compile_definitions(ayni_normal_fixture_tests PRIVATE
    AYNI_NORMAL_ARTIFACT_ROOT="${CMAKE_CURRENT_BINARY_DIR}/normal-artifacts")
gtest_discover_tests(ayni_normal_fixture_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
add_test(NAME NormalFixtures.artifacts_replay COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/normal_artifacts_test.py" $<TARGET_FILE:ayni-sim>
    "${CMAKE_CURRENT_BINARY_DIR}/normal-artifacts")
set_tests_properties(NormalFixtures.artifacts_replay PROPERTIES TIMEOUT 120)
add_test(NAME MultinodeNormal.artifacts_replay COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/multinode_artifacts_test.py" $<TARGET_FILE:ayni-sim>)
set_tests_properties(MultinodeNormal.artifacts_replay PROPERTIES TIMEOUT 120)

add_executable(ayni_experiment_tests experiment_fixture_test.cpp replacement_policy_test.cpp)
target_include_directories(ayni_experiment_tests PRIVATE "${AYNI_ROOT}/runtime/sim_node")
target_link_libraries(ayni_experiment_tests PRIVATE ayni_sim_experiments ayni_experiment_policy ayni_strategies ayni_provider_mock_execution ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_experiment_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
add_test(NAME ExperimentFixtures.artifacts_replay COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/experiment_artifacts_test.py" $<TARGET_FILE:ayni-sim>)
set_tests_properties(ExperimentFixtures.artifacts_replay PROPERTIES TIMEOUT 120)

# Same public application and real Native storage as the independent ESP consumer.
execute_process(COMMAND git -C "${AYNI_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE portable_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT portable_revision)
    set(portable_revision "unversioned")
endif()
add_test(NAME portable_independent_normal
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tests/portability/run_native.py"
        --source "${AYNI_ROOT}" --sanitize "${AYNI_SANITIZE}"
        --build-type "${CMAKE_BUILD_TYPE}" --generator "${CMAKE_GENERATOR}"
        --source-revision "${portable_revision}")
set_tests_properties(portable_independent_normal PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
