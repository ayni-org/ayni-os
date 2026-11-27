cmake_minimum_required(VERSION 3.22)
project(ayni_wifi_contract_checks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(AYNI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${AYNI_ROOT}/cmake/AyniSDK.cmake")
ayni_add_sdk(COMPONENTS core conversation permit_gate model_protocol operation_session)
include("${AYNI_ROOT}/cmake/AyniYyjson.cmake")
include("${AYNI_ROOT}/cmake/AyniGoogleTest.cmake")
include("${AYNI_ROOT}/cmake/AyniMarkdown.cmake")
enable_testing()
add_subdirectory("${AYNI_ROOT}/hosts/sim" "sim-host")
add_executable(ayni_wifi_checks codec_test.cpp preferences_test.cpp reply_voice_test.cpp app_menu_test.cpp nvs_stub.cpp
    ../../firmware/main/tool_outline.cpp
    ../../firmware/wifi/model_codec.cpp
    ../../firmware/wifi/settings_codec.cpp ../../firmware/wifi/settings.cpp ../../firmware/wifi/capture.cpp
    ../../firmware/main/motor_check.cpp ../../firmware/main/reply_voice.cpp ../../firmware/main/speech_text.cpp ../../firmware/wifi/preferences.cpp
    model_buffer_test.cpp reply_test.cpp speech_stream_test.cpp speech_request_test.cpp pcm_buffer_test.cpp turn_status_test.cpp reset_plan_test.cpp motor_check_test.cpp diagnostic_test.cpp update_plan_test.cpp network_time_test.cpp response_limit_test.cpp)
target_include_directories(ayni_wifi_checks PRIVATE stubs ../../firmware/wifi ../../firmware/main)
target_compile_definitions(ayni_wifi_checks PRIVATE CONFIG_AYNI_WIFI_CHAT=1)
target_link_libraries(ayni_wifi_checks PRIVATE Ayni::Core Ayni::Strategies Ayni::Conversation Ayni::PermitGate Ayni::ModelProtocol ayni_host_sim yyjson ayni_markdown GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(ayni_wifi_checks
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
add_subdirectory("${AYNI_ROOT}/tests/integration/esp32_wifi_chat" "esp32-integration")
