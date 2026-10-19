add_library(ayni_host_sim STATIC sim_host.cpp)
target_include_directories(ayni_host_sim PUBLIC include)
# Links Core for ayni_completion_post, which it calls to complete storage.
target_link_libraries(ayni_host_sim PUBLIC ayni_api PRIVATE ayni_core ayni_warnings)
