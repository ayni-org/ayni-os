find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_test(NAME NativeCli.scripted_sessions COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/native_cli_test.py $<TARGET_FILE:ayni>)
set_tests_properties(NativeCli.scripted_sessions PROPERTIES TIMEOUT 90)
