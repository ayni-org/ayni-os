add_library(ayni_assistant_application STATIC assistant.cpp goal_requests.cpp goals.cpp memory_organize.cpp scene_observe.cpp
    schedule.cpp schedule_tool.cpp)
target_include_directories(ayni_assistant_application PUBLIC include)
target_link_libraries(ayni_assistant_application PUBLIC ayni_api PRIVATE ayni_core ayni_warnings)
