add_library(ayni_host_storage STATIC audit_sink.cpp file_log.cpp request.cpp file_value.cpp)
target_include_directories(ayni_host_storage PUBLIC include)
target_link_libraries(ayni_host_storage PUBLIC ayni_api PRIVATE ayni_core ayni_warnings)
# IDF appends its own dialect after CMake's standard flag; this source target
# keeps the same C++17 implementation contract as Core and strategies.
target_compile_options(ayni_host_storage PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
