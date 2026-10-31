# Consumers deliberately compile without exceptions/RTTI and see no Core-private headers.
foreach(example IN ITEMS new_hardware new_model new_strategy composite_workflow managed_operations)
    add_executable(ayni_example_${example} ${example}.cpp)
    target_link_libraries(ayni_example_${example} PRIVATE ayni_api ayni_runtime_examples ayni_core_options)
    if(AYNI_BUILD_TESTS)
        add_test(NAME Examples.${example} COMMAND ayni_example_${example})
        set_tests_properties(Examples.${example} PROPERTIES TIMEOUT 30)
    endif()
endforeach()
if(AYNI_BUILD_TESTS)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME Examples.managed_operations_native COMMAND "${Python3_EXECUTABLE}"
        "${AYNI_ROOT}/tests/integration/sdk_hosts_test.py" $<TARGET_FILE:ayni_example_managed_operations>)
    set_tests_properties(Examples.managed_operations_native PROPERTIES TIMEOUT 30)
    add_test(NAME Examples.assistant_demo COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/assistant_demo.sh $<TARGET_FILE:ayni>)
    set_tests_properties(Examples.assistant_demo PROPERTIES TIMEOUT 90)
endif()
