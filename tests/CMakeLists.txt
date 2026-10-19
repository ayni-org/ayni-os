include("${AYNI_ROOT}/cmake/AyniGoogleTest.cmake")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

add_subdirectory(unit)
add_subdirectory(contract)
add_subdirectory(tools)
