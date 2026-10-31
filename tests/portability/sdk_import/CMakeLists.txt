cmake_minimum_required(VERSION 3.22)
project(ayni_independent_consumer LANGUAGES C CXX)

if(NOT AYNI_SOURCE_DIR)
    message(FATAL_ERROR "Set AYNI_SOURCE_DIR to the read-only Ayni source dependency")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include("${AYNI_SOURCE_DIR}/cmake/AyniSDK.cmake")
ayni_add_sdk(COMPONENTS core strategies)
ayni_add_sdk(COMPONENTS core strategies)

if(PROBE_COMPONENTS)
    ayni_add_sdk(COMPONENTS ${PROBE_COMPONENTS})
endif()
if(PROBE_SECOND_SOURCE)
    include("${PROBE_SECOND_SOURCE}/cmake/AyniSDK.cmake")
endif()
set(forbidden_targets SQLite3::SQLite3 CURL::libcurl ayni_runtime_examples ayni_runtime_native_node)
if(NOT native_host IN_LIST PROBE_COMPONENTS)
    list(APPEND forbidden_targets Threads::Threads ayni_host_native)
endif()
foreach(forbidden IN LISTS forbidden_targets)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "The portable SDK imported deployment target ${forbidden}")
    endif()
endforeach()

add_executable(consumer_c c_consumer.c)
target_link_libraries(consumer_c PRIVATE Ayni::Core)
set_target_properties(consumer_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(consumer_c PRIVATE -pedantic-errors)

add_executable(consumer_cpp cpp_consumer.cpp)
target_link_libraries(consumer_cpp PRIVATE Ayni::Core Ayni::Strategies)
target_compile_options(consumer_cpp PRIVATE -fno-exceptions -fno-rtti)

# Exercise optional archive symbols as a consumer, rather than only checking aliases.
foreach(pair IN ITEMS "software_provider;SoftwareProvider;SOFTWARE"
        "permit_gate;PermitGate;GATE" "assistant;Assistant;ASSISTANT"
        "software_workflow;SoftwareWorkflow;WORKFLOW" "mock_provider;MockProvider;MOCK"
        "mock_execution;MockExecution;EXECUTION" "host_storage;HostStorage;STORAGE"
        "native_host;NativeHost;NATIVE_HOST")
    list(GET pair 0 component)
    list(GET pair 1 alias)
    list(GET pair 2 definition)
    if(component IN_LIST PROBE_COMPONENTS)
        target_link_libraries(consumer_cpp PRIVATE Ayni::${alias})
        target_compile_definitions(consumer_cpp PRIVATE PROBE_${definition})
    endif()
endforeach()

enable_testing()
add_test(NAME public_c COMMAND consumer_c)
add_test(NAME public_cpp COMMAND consumer_cpp)
