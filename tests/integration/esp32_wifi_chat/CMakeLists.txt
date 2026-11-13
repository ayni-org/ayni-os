# Public Core/Sim checks for the independent ESP deployment. Root ASan CI and the
# standalone Wi-Fi checks build this same target without widening experiment dependencies.
include("${AYNI_ROOT}/cmake/AyniMarkdown.cmake")
add_executable(ayni_esp32_chat_integration_tests
    configuration_test.cpp reply_voice_kernel_test.cpp speech_text_test.cpp
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/reply_voice.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/speech_text.cpp")
target_include_directories(ayni_esp32_chat_integration_tests PRIVATE
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main")
target_link_libraries(ayni_esp32_chat_integration_tests PRIVATE
    Ayni::Core Ayni::Strategies Ayni::ModelProtocol ayni_host_sim ayni_markdown ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_esp32_chat_integration_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
