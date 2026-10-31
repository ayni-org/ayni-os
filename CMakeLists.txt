cmake_minimum_required(VERSION 3.22)

project(ayni VERSION 0.1.0 LANGUAGES C CXX)

option(AYNI_BUILD_TESTS "Build unit, contract and tool tests" OFF)
set(AYNI_HOST "native" CACHE STRING "Host built by this CMake project")
set_property(CACHE AYNI_HOST PROPERTY STRINGS native)
set(AYNI_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined")

if(NOT AYNI_HOST STREQUAL "native")
    message(FATAL_ERROR
        "AYNI_HOST=${AYNI_HOST} is not supported here. Only 'native' builds with "
        "this project; ESP32 firmware builds with idf.py in targets/esp32_assistant.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(AYNI_SANITIZE)
    add_compile_options(-fsanitize=${AYNI_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${AYNI_SANITIZE})
endif()

include(cmake/AyniSDK.cmake)
ayni_add_sdk(COMPONENTS core strategies software_provider permit_gate assistant
    software_workflow mock_provider mock_execution native_host)
find_package(SQLite3 3.31 REQUIRED) # Native composition/history only; Core never links SQLite.

add_subdirectory(hosts/sim)
add_subdirectory(providers/presentation)
add_subdirectory(providers/openai_compat)
add_subdirectory(providers/shell)
add_subdirectory(applications/normal_fixtures)
add_subdirectory(applications/experiment_fixtures)
add_subdirectory(runtime/common)
add_subdirectory(runtime/native_node)
add_subdirectory(runtime/sim_node)
add_subdirectory(runtime/examples)
add_subdirectory(targets/simulator)
add_subdirectory(targets/native_cli)

if(AYNI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
