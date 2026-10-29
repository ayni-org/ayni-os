add_library(ayni_provider_shell STATIC shell.cpp process.cpp)
target_include_directories(ayni_provider_shell PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(ayni_provider_shell PUBLIC ayni_api PRIVATE ayni_core ayni_warnings Threads::Threads)
