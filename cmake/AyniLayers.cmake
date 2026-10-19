# Core module targets and their dependency direction (ADR-0008, ADR-0011).
# Layers come from tools/architecture.json, the same file tools/check_deps.py
# reads, so the link graph and the include check cannot disagree.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/AyniCommon.cmake")

set(_ayni_architecture_file "${AYNI_ROOT}/tools/architecture.json")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${_ayni_architecture_file}")
file(READ "${_ayni_architecture_file}" _ayni_architecture)

string(JSON _ayni_layer_count LENGTH "${_ayni_architecture}" core_layers)
math(EXPR _ayni_last_layer "${_ayni_layer_count} - 1")
foreach(_ayni_layer RANGE ${_ayni_last_layer})
    string(JSON _ayni_module_count LENGTH "${_ayni_architecture}" core_layers ${_ayni_layer})
    math(EXPR _ayni_last_module "${_ayni_module_count} - 1")
    foreach(_ayni_index RANGE ${_ayni_last_module})
        string(JSON _ayni_module GET "${_ayni_architecture}" core_layers ${_ayni_layer} ${_ayni_index})
        set_property(GLOBAL PROPERTY AYNI_CORE_LAYER_${_ayni_module} ${_ayni_layer})
    endforeach()
endforeach()

# Include root for Core-internal headers ("work/task/task.h"). Core modules
# link it privately, so it never reaches targets outside Core.
add_library(ayni_core_internal INTERFACE)
target_include_directories(ayni_core_internal INTERFACE "${AYNI_ROOT}/core")

# ayni_add_core_module(<module> [SOURCES <file>...] [DEPENDS <module>...])
#
# Creates ayni_core_<module>. Layer 0 (kernel) is the top; a module may depend
# only on modules in a strictly lower layer, which also forbids dependencies
# between modules of the same layer.
function(ayni_add_core_module module)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ayni_add_core_module(${module}): unexpected arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()

    get_property(layer GLOBAL PROPERTY AYNI_CORE_LAYER_${module})
    if("${layer}" STREQUAL "")
        message(FATAL_ERROR "core/${module} is not a module listed in tools/architecture.json")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "core/${module} has no sources; add the target when the module gets code")
    endif()

    set(dependency_targets)
    foreach(dependency IN LISTS arg_DEPENDS)
        get_property(dependency_layer GLOBAL PROPERTY AYNI_CORE_LAYER_${dependency})
        if("${dependency_layer}" STREQUAL "")
            message(FATAL_ERROR "core/${module} depends on unknown module '${dependency}'")
        endif()
        if(NOT dependency_layer GREATER layer)
            message(FATAL_ERROR
                "core/${module} (layer ${layer}) may not depend on core/${dependency} "
                "(layer ${dependency_layer}): dependencies point to strictly lower layers only")
        endif()
        list(APPEND dependency_targets ayni_core_${dependency})
    endforeach()

    add_library(ayni_core_${module} STATIC ${arg_SOURCES})
    target_link_libraries(ayni_core_${module}
        PUBLIC ayni_api ${dependency_targets}
        PRIVATE ayni_core_internal ayni_core_options)
endfunction()
