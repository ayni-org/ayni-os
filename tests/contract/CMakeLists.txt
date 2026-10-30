# Header self-containment: every public/draft C header compiles twice as strict
# C99 and C++17. Host/runtime headers and the separately checked SDK facade are
# C++ only. A header that fails breaks the build.
file(GLOB_RECURSE ayni_c_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}/include" "${AYNI_ROOT}/include/ayni/*.h")
file(GLOB_RECURSE ayni_cxx_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}" "${AYNI_ROOT}/hosts/native/include/*.h" "${AYNI_ROOT}/hosts/sim/include/*.h"
    "${AYNI_ROOT}/runtime/native_node/include/*.h"
    "${AYNI_ROOT}/runtime/sim_node/include/*.h"
    "${AYNI_ROOT}/runtime/common/include/*.h"
    "${AYNI_ROOT}/runtime/examples/include/*.h"
    "${AYNI_ROOT}/providers/mock/include/*.h"
    "${AYNI_ROOT}/providers/mock_execution/include/*.h"
    "${AYNI_ROOT}/providers/presentation/include/*.h"
    "${AYNI_ROOT}/providers/shell/include/*.h"
    "${AYNI_ROOT}/applications/assistant/include/*.h"
    "${AYNI_ROOT}/applications/normal_fixtures/include/*.h"
    "${AYNI_ROOT}/strategies/builtin/include/*.h")

set(header_check_sources)
function(ayni_header_check_source header extension)
    string(MAKE_C_IDENTIFIER "${header}" name)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_checks/${name}.${extension}")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${header}>\n#include <${header}>\n")
    set(header_check_sources ${header_check_sources} "${source}" PARENT_SCOPE)
endfunction()

foreach(header IN LISTS ayni_c_headers)
    ayni_header_check_source("${header}" c)
    ayni_header_check_source("${header}" cpp)
endforeach()

# All public and draft headers together, so that two headers cannot define
# the same name differently.
list(TRANSFORM ayni_c_headers REPLACE "^(.+)$" "#include <\\1>" OUTPUT_VARIABLE all_includes)
list(JOIN all_includes "\n" all_includes)
foreach(extension c cpp)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_checks/all_public_headers.${extension}")
    file(CONFIGURE OUTPUT "${source}" CONTENT "${all_includes}\n")
    list(APPEND header_check_sources "${source}")
endforeach()
foreach(header IN LISTS ayni_cxx_headers)
    string(REGEX REPLACE "^.*/include/" "" header "${header}")
    ayni_header_check_source("${header}" cpp)
endforeach()

ayni_header_check_source("ayni/values.hpp" cpp)
ayni_header_check_source("ayni/values_json.hpp" cpp)
ayni_header_check_source("ayni/detail/tool_usage.hpp" cpp)

# abi_layout.c pins the published struct layouts at compile time (C-VER-002).
add_library(ayni_header_checks OBJECT ${header_check_sources} abi_layout.c)
target_link_libraries(ayni_header_checks PRIVATE ayni_api ayni_host_native ayni_host_sim ayni_runtime_native_node ayni_runtime_sim_node ayni_runtime_common ayni_runtime_examples ayni_provider_mock ayni_provider_mock_execution ayni_strategies ayni_assistant_application ayni_normal_fixture_policy ayni_provider_presentation ayni_provider_shell ayni_warnings)
target_compile_options(ayni_header_checks PRIVATE $<$<COMPILE_LANGUAGE:C>:-pedantic-errors>)

# The implemented ABI is callable and linkable from a plain C99 program.
add_executable(ayni_c_abi_smoke c_abi_smoke.c)
target_link_libraries(ayni_c_abi_smoke PRIVATE ayni_core ayni_warnings)
target_compile_options(ayni_c_abi_smoke PRIVATE -pedantic-errors)
add_test(NAME c_abi_create_destroy_from_c99 COMMAND ayni_c_abi_smoke)

add_executable(ayni_cpp_sdk_smoke cpp_sdk_smoke.cpp)
target_link_libraries(ayni_cpp_sdk_smoke PRIVATE ayni_core ayni_core_options)
add_test(NAME cpp_sdk_without_exceptions_or_rtti COMMAND ayni_cpp_sdk_smoke)

# Core takes all memory from its caller (ADR-0014): no Core archive may
# reference a heap allocator. The probe cases prove the check detects a
# direct allocator call and a libc function that allocates for its caller.
get_property(core_module_targets GLOBAL PROPERTY AYNI_CORE_MODULE_TARGETS)
set(core_archives)
foreach(target IN LISTS core_module_targets)
    list(APPEND core_archives "$<TARGET_FILE:${target}>")
endforeach()
string(JOIN "," core_archives ${core_archives})
add_test(NAME core_does_not_reference_heap_allocator
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DARCHIVES=${core_archives}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_heap.cmake")

add_library(ayni_heap_probe STATIC heap_probe.c)
target_link_libraries(ayni_heap_probe PRIVATE ayni_warnings)
add_test(NAME heap_check_detects_allocator
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DARCHIVES=$<TARGET_FILE:ayni_heap_probe>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_heap.cmake")
set_tests_properties(heap_check_detects_allocator PROPERTIES
    PASS_REGULAR_EXPRESSION "Core references heap allocator symbols: malloc")

add_library(ayni_heap_probe_strdup STATIC heap_probe_strdup.c)
target_link_libraries(ayni_heap_probe_strdup PRIVATE ayni_warnings)
add_test(NAME heap_check_detects_strdup
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DARCHIVES=$<TARGET_FILE:ayni_heap_probe_strdup>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_heap.cmake")
set_tests_properties(heap_check_detects_strdup PROPERTIES
    PASS_REGULAR_EXPRESSION "Core references heap allocator symbols: (__)?strdup")

# The executor gate must work without linking any Core implementation archive.
add_executable(ayni_control_c_abi_smoke control_c_abi_smoke.c)
target_link_libraries(ayni_control_c_abi_smoke PRIVATE ayni_control_gate ayni_warnings)
target_compile_options(ayni_control_c_abi_smoke PRIVATE -pedantic-errors)
add_test(NAME control_gate_from_c99_without_core COMMAND ayni_control_c_abi_smoke)
add_test(NAME control_gate_does_not_reference_heap_allocator
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DARCHIVES=$<TARGET_FILE:ayni_control_gate>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_no_heap.cmake")
