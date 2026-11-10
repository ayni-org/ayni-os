include("${AYNI_ROOT}/cmake/AyniYyjson.cmake")
add_library(ayni_model_protocol STATIC protocol.cpp json.cpp)
target_include_directories(ayni_model_protocol PUBLIC include)
target_link_libraries(ayni_model_protocol PUBLIC ayni_api PRIVATE ayni_core ayni_warnings yyjson)
