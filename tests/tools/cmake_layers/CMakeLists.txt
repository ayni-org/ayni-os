# Probe project for the layer tests in ../CMakeLists.txt.
cmake_minimum_required(VERSION 3.22)
project(ayni_layer_probe LANGUAGES C CXX)

include("${AYNI_REPOSITORY}/cmake/AyniLayers.cmake")

# PROBE_DEPENDS is comma-separated; semicolons do not survive ctest arguments.
string(REPLACE "," ";" PROBE_DEPENDS "${PROBE_DEPENDS}")
foreach(dependency IN LISTS PROBE_DEPENDS)
    get_property(known GLOBAL PROPERTY AYNI_CORE_LAYER_${dependency})
    if(NOT "${known}" STREQUAL "")
        ayni_add_core_module(${dependency} SOURCES probe.cpp)
    endif()
endforeach()
ayni_add_core_module(${PROBE_MODULE} SOURCES probe.cpp DEPENDS ${PROBE_DEPENDS})
