# Public Core/Sim checks for the independent ESP deployment. Root ASan CI and the
# standalone Wi-Fi checks build this same target without widening experiment dependencies.
include("${AYNI_ROOT}/cmake/AyniMarkdown.cmake")
add_executable(ayni_esp32_chat_integration_tests
    configuration_test.cpp reply_voice_kernel_test.cpp speech_text_test.cpp
    turn_diagnostics_test.cpp model_delivery_test.cpp
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/transport.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/wifi/model_delivery.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/reply_voice.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/speech_text.cpp")
target_include_directories(ayni_esp32_chat_integration_tests PRIVATE
    transport_shim
    "${AYNI_ROOT}/tests/portability/esp_files/shim"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main"
    "${AYNI_ROOT}/hosts/esp_idf/include")
target_link_libraries(ayni_esp32_chat_integration_tests PRIVATE
    Ayni::Core Ayni::Strategies Ayni::Conversation Ayni::ModelProtocol ayni_host_sim ayni_markdown ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_esp32_chat_integration_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
