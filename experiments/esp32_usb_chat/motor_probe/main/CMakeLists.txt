idf_component_register(SRCS "probe.cpp" INCLUDE_DIRS "../../firmware/main"
    PRIV_REQUIRES esp_driver_rmt esp_driver_uart esp_driver_gpio esp_driver_pcnt esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
