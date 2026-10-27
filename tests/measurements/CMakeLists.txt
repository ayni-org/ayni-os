# Manual measurements are separate from the correctness suite. CI exercises a
# small workload and validates its output, without asserting host timing.
add_executable(ayni_native_measurements native_work.cpp)
target_include_directories(ayni_native_measurements PRIVATE ../unit "${AYNI_ROOT}/tools")
target_link_libraries(ayni_native_measurements PRIVATE ayni_core ayni_core_internal
    ayni_host_sim ayni_warnings)
target_compile_definitions(ayni_native_measurements PRIVATE
    AYNI_MEASURE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" AYNI_MEASURE_SANITIZER="${AYNI_SANITIZE}")
set(measurement_metadata "${CMAKE_CURRENT_BINARY_DIR}/generated/measurement_metadata.h")
add_custom_target(ayni_generate_measurement_metadata
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${AYNI_ROOT}"
        "-DOUTPUT=${measurement_metadata}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_metadata.cmake"
    BYPRODUCTS "${measurement_metadata}" VERBATIM)
add_dependencies(ayni_native_measurements ayni_generate_measurement_metadata)
target_include_directories(ayni_native_measurements PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(ayni_memory_measurements "${AYNI_ROOT}/tools/measure_kernel_memory.c")
target_link_libraries(ayni_memory_measurements PRIVATE ayni_core ayni_warnings)
add_test(NAME NativeMeasurements.small_workload COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/check_measurement.py"
    $<TARGET_FILE:ayni_native_measurements> $<TARGET_FILE:ayni_memory_measurements>)
set_tests_properties(NativeMeasurements.small_workload PROPERTIES TIMEOUT 60)
