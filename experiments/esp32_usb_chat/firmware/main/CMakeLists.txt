idf_component_register(SRCS "main.cpp" "transport.cpp" "display.cpp" "chat.cpp" "screen.cpp" "speaker.cpp" "policy.cpp" "model_tools.cpp" "motor.cpp" "motor_output.cpp" "motor_execution.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_idf esp_timer heap esp_psram spi_flash log freertos
        esp_lcd esp_driver_spi esp_driver_gpio esp_driver_uart esp_driver_i2s esp_driver_rmt esp_driver_pcnt)
if(NOT CONFIG_SPIRAM_XIP_FROM_PSRAM OR CONFIG_FREERTOS_SMP)
    message(FATAL_ERROR "This experiment requires PSRAM XiP and the legacy IDF kernel")
endif()
include("${AYNI_SOURCE_DIR}/cmake/AyniSDK.cmake")
ayni_add_sdk(COMPONENTS core permit_gate)
target_link_libraries(${COMPONENT_LIB} PRIVATE Ayni::Core Ayni::PermitGate)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -std=c++17 -fno-exceptions -fno-rtti)
