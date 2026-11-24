add_library(ayni_control_gate STATIC gate.cpp envelope.cpp session.cpp)
target_link_libraries(ayni_control_gate PUBLIC ayni_api PRIVATE ayni_core_options)
