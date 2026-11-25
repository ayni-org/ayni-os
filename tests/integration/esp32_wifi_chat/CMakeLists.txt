# Public Core/Sim checks for the independent ESP deployment. Root ASan CI and the
# standalone Wi-Fi checks build this same target without widening experiment dependencies.
include("${AYNI_ROOT}/cmake/AyniMarkdown.cmake")
add_executable(ayni_esp32_chat_integration_tests
    configuration_test.cpp reply_voice_kernel_test.cpp speech_text_test.cpp
    turn_diagnostics_test.cpp transport_test_support.cpp model_delivery_test.cpp control_mailbox_test.cpp motor_registration_test.cpp
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor_sessions.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/transport.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/wifi/model_delivery.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/reply_voice.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/speech_text.cpp")
target_include_directories(ayni_esp32_chat_integration_tests PRIVATE
    transport_shim "${AYNI_ROOT}/tests/unit"
    "${AYNI_ROOT}/tests/portability/esp_files/shim"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main"
    "${AYNI_ROOT}/hosts/esp_idf/include")
target_link_libraries(ayni_esp32_chat_integration_tests PRIVATE
    Ayni::Core Ayni::OperationSession ayni_core_internal Ayni::Strategies Ayni::Conversation Ayni::ModelProtocol ayni_host_sim ayni_markdown ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_esp32_chat_integration_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})

# Exercise production executor/Chat logic with MotorOutput, Display and ESP Host substitutes.
# RMT/DMA implementation and physical waveform timing are outside this host target.
add_executable(ayni_esp32_motor_execution_tests motor_execution_test.cpp chat_tick_test.cpp transport_test_support.cpp
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/chat.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/screen.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor_sessions.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor_check.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/reply_voice.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/speech_text.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor_execution.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/motor_worker.cpp"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main/transport.cpp")
target_include_directories(ayni_esp32_motor_execution_tests PRIVATE transport_shim
    "${AYNI_ROOT}/tests/portability/esp_files/shim"
    "${AYNI_ROOT}/experiments/esp32_wifi_chat/firmware/main"
    "${AYNI_ROOT}/hosts/esp_idf/include" "${AYNI_ROOT}/tests/unit")
target_link_libraries(ayni_esp32_motor_execution_tests PRIVATE Ayni::Core Ayni::PermitGate
    Ayni::ModelProtocol Ayni::OperationSession Ayni::Conversation Ayni::Strategies ayni_markdown
    ayni_core_internal ayni_host_sim ayni_warnings GTest::gtest_main)
gtest_discover_tests(ayni_esp32_motor_execution_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT ${AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS})
