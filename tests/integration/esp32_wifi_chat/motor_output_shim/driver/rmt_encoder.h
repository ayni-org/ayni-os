#pragma once
#include <cstdint>
#include <esp_err.h>
struct TestEncoder {};
using rmt_encoder_handle_t = TestEncoder *;
struct rmt_symbol_word_t {
    uint32_t level0 = 0, duration0 = 0, level1 = 0, duration1 = 0;
};
struct rmt_copy_encoder_config_t {};
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);
