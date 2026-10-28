# Consumers deliberately compile without exceptions/RTTI and see no Core-private headers.
foreach(example IN ITEMS new_hardware new_model new_strategy composite_workflow)
    add_executable(ayni_example_${example} ${example}.cpp)
    target_link_libraries(ayni_example_${example} PRIVATE ayni_api ayni_runtime_examples ayni_core_options)
    if(AYNI_BUILD_TESTS)
        add_test(NAME Examples.${example} COMMAND ayni_example_${example})
        set_tests_properties(Examples.${example} PROPERTIES TIMEOUT 30)
    endif()
endforeach()
if(AYNI_BUILD_TESTS)
    add_test(NAME Examples.assistant_demo COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/assistant_demo.sh $<TARGET_FILE:ayni>)
    set_tests_properties(Examples.assistant_demo PROPERTIES TIMEOUT 90)
endif()
