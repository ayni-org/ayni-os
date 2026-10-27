include("${AYNI_ROOT}/cmake/AyniGoogleTest.cmake")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
include(GoogleTest)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(contract)
add_subdirectory(tools)
add_subdirectory(measurements)
