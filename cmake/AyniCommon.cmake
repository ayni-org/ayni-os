# Compile settings and the public API target shared by every Ayni CMake
# build, including Core when ESP-IDF adds core/ as a subdirectory (ADR-0011).

include_guard(GLOBAL)

# A cache entry, not a normal variable: ESP-IDF includes this file from one
# component's scope and Core reads AYNI_ROOT from another.
get_filename_component(_ayni_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(AYNI_ROOT "${_ayni_root}" CACHE INTERNAL "Ayni repository root")

option(AYNI_WARNINGS_AS_ERRORS "Treat warnings in Ayni sources as errors" ON)

if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$"
   OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR
        "Ayni supports GCC and Clang only; found C='${CMAKE_C_COMPILER_ID}' "
        "CXX='${CMAKE_CXX_COMPILER_ID}'. The warning and ABI flags below are "
        "not defined for other compilers.")
endif()

# Warnings for Ayni sources that include vendor SDK headers (ESP-IDF Host and
# runtime). Those headers use GCC extensions, so -Wpedantic is left out here.
add_library(ayni_platform_warnings INTERFACE)
target_compile_options(ayni_platform_warnings INTERFACE
    -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${AYNI_WARNINGS_AS_ERRORS}>:-Werror>)

# Warnings for every other Ayni source file. Third-party code links neither.
add_library(ayni_warnings INTERFACE)
target_compile_options(ayni_warnings INTERFACE -Wpedantic)
target_link_libraries(ayni_warnings INTERFACE ayni_platform_warnings)

# Core and the future C++ wrapper: C++17 without exceptions or RTTI, so the
# same code builds for hosts that cannot afford either (plan section 8).
add_library(ayni_core_options INTERFACE)
target_compile_features(ayni_core_options INTERFACE cxx_std_17)
target_compile_options(ayni_core_options INTERFACE
    # ESP-IDF's directory defaults can append -std=gnu++26 after CMake's
    # standard flag. Keep Ayni implementation sources on the promised C++17.
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17 -fno-exceptions -fno-rtti>)
target_link_libraries(ayni_core_options INTERFACE ayni_warnings)

# The implemented public C ABI (include/ayni/ayni.h) and the draft headers.
# Linking this target gives no symbols; Core provides them.
add_library(ayni_api INTERFACE)
target_include_directories(ayni_api INTERFACE "${AYNI_ROOT}/include")
target_compile_features(ayni_api INTERFACE c_std_99 cxx_std_17)
