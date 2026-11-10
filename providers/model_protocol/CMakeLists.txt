include("${AYNI_ROOT}/cmake/AyniYyjson.cmake")
add_library(ayni_model_protocol STATIC protocol.cpp request.cpp stream.cpp json.cpp)
target_compile_features(ayni_model_protocol PUBLIC cxx_std_17)
# ESP-IDF directory defaults may otherwise append gnu++26 after CMake's standard.
target_compile_options(ayni_model_protocol PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_include_directories(ayni_model_protocol PUBLIC include)
target_link_libraries(ayni_model_protocol PUBLIC ayni_api PRIVATE ayni_core ayni_warnings yyjson)
