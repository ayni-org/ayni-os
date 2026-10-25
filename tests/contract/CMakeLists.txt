# Header self-containment: every public and draft header compiles on its own,
# twice in one translation unit, as strict C99 and as C++17. Host and runtime
# headers are C++ only. A header that fails breaks the build.
file(GLOB_RECURSE ayni_c_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}/include" "${AYNI_ROOT}/include/ayni/*.h")
file(GLOB_RECURSE ayni_cxx_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}" "${AYNI_ROOT}/hosts/native/include/*.h" "${AYNI_ROOT}/hosts/sim/include/*.h"
    "${AYNI_ROOT}/runtime/native_node/include/*.h"
    "${AYNI_ROOT}/providers/mock/include/*.h")

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

# abi_layout.c pins the published struct layouts at compile time (C-VER-002).
add_library(ayni_header_checks OBJECT ${header_check_sources} abi_layout.c)
target_link_libraries(ayni_header_checks PRIVATE ayni_api ayni_host_native ayni_host_sim ayni_runtime_native_node ayni_provider_mock ayni_warnings)
target_compile_options(ayni_header_checks PRIVATE $<$<COMPILE_LANGUAGE:C>:-pedantic-errors>)

# The implemented ABI is callable and linkable from a plain C99 program.
add_executable(ayni_c_abi_smoke c_abi_smoke.c)
target_link_libraries(ayni_c_abi_smoke PRIVATE ayni_core ayni_warnings)
target_compile_options(ayni_c_abi_smoke PRIVATE -pedantic-errors)
add_test(NAME c_abi_create_destroy_from_c99 COMMAND ayni_c_abi_smoke)

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
