# Header self-containment: every public and draft header compiles on its own,
# twice in one translation unit, as strict C99 and as C++17. Host and runtime
# headers are C++ only. A header that fails breaks the build.
file(GLOB_RECURSE ayni_c_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}/include" "${AYNI_ROOT}/include/ayni/*.h")
file(GLOB_RECURSE ayni_cxx_headers CONFIGURE_DEPENDS
    RELATIVE "${AYNI_ROOT}" "${AYNI_ROOT}/hosts/native/include/*.h" "${AYNI_ROOT}/runtime/native_node/include/*.h")

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
foreach(header IN LISTS ayni_cxx_headers)
    string(REGEX REPLACE "^.*/include/" "" header "${header}")
    ayni_header_check_source("${header}" cpp)
endforeach()

add_library(ayni_header_checks OBJECT ${header_check_sources})
target_link_libraries(ayni_header_checks PRIVATE ayni_api ayni_host_native ayni_runtime_native_node ayni_warnings)
target_compile_options(ayni_header_checks PRIVATE $<$<COMPILE_LANGUAGE:C>:-pedantic-errors>)

# The implemented ABI is callable and linkable from a plain C99 program.
add_executable(ayni_c_abi_smoke c_abi_smoke.c)
target_link_libraries(ayni_c_abi_smoke PRIVATE ayni_core ayni_warnings)
target_compile_options(ayni_c_abi_smoke PRIVATE -pedantic-errors)
add_test(NAME c_abi_create_destroy_from_c99 COMMAND ayni_c_abi_smoke)
