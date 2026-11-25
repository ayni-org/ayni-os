set(wifi_sources)
set(wifi_dependencies)
if(AYNI_WIFI_PROFILE)
    set(wifi_sources "../wifi/runtime.cpp" "../wifi/preferences.cpp" "../wifi/app_menu.cpp" "../wifi/reset_worker.cpp" "../wifi/canvas.cpp" "../wifi/settings.cpp"
        "../wifi/touch.cpp" "../wifi/station.cpp" "../wifi/remote.cpp" "../wifi/api.cpp" "../wifi/model_delivery.cpp" "../wifi/model_codec.cpp" "../wifi/web_config.cpp" "../wifi/ota_update.cpp" "../wifi/settings_codec.cpp" "../wifi/capture.cpp" "../wifi/motor_panel.cpp")
    set(wifi_dependencies bt nvs_flash esp_wifi esp_netif esp_http_client mbedtls
        esp_driver_i2c nimble_central_utils esp_http_server app_update)
endif()
idf_component_register(SRCS "main.cpp" "transport.cpp" "display.cpp" "chat.cpp" "screen.cpp" "speaker.cpp" "tool_outline.cpp" "motor.cpp" "motor_sessions.cpp" "motor_check.cpp" "reply_voice.cpp" "speech_text.cpp" "motor_execution.cpp" "motor_output.cpp" "motor_worker.cpp" ${wifi_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_idf esp_timer heap esp_psram spi_flash log freertos pthread
        esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2s esp_driver_rmt esp_driver_pcnt ${wifi_dependencies})
if(NOT CONFIG_SPIRAM_XIP_FROM_PSRAM OR CONFIG_FREERTOS_SMP)
    message(FATAL_ERROR "This experiment requires PSRAM XiP and the legacy IDF kernel")
endif()
if(AYNI_WIFI_PROFILE AND NOT CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL EQUAL 0)
    message(FATAL_ERROR "Wi-Fi retained bodies require PSRAM-first default allocation; regenerate sdkconfig from the tracked defaults")
endif()
include("${AYNI_SOURCE_DIR}/cmake/AyniSDK.cmake")
include("${AYNI_SOURCE_DIR}/cmake/AyniMarkdown.cmake")
ayni_add_sdk(COMPONENTS core conversation permit_gate model_protocol operation_session)
target_link_libraries(${COMPONENT_LIB} PRIVATE Ayni::Core Ayni::Strategies Ayni::Conversation Ayni::PermitGate Ayni::ModelProtocol Ayni::OperationSession ayni_markdown)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -std=c++17 -fno-exceptions -fno-rtti)
if(AYNI_WIFI_PROFILE)
    # IDF's early dependency scan does not import arbitrary project cache
    # variables. These components are present in the full-profile build; attach
    # their usage requirements after the selected profile is known.
    idf_component_optional_requires(PRIVATE ${wifi_dependencies})
    target_link_libraries(${COMPONENT_LIB} PRIVATE yyjson)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../../build/assets/ayni_font.bin" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../wifi/config.html" TEXT)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../wifi/diagnostics.html" TEXT)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../wifi/firmware.html" TEXT)
endif()
