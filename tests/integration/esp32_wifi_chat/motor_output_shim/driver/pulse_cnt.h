#pragma once
#include <esp_err.h>
struct TestCounter;
using pcnt_unit_handle_t = TestCounter *;
using pcnt_channel_handle_t = TestCounter *;
inline constexpr int PCNT_CLK_SRC_DEFAULT = 0, PCNT_CHANNEL_EDGE_ACTION_INCREASE = 1,
                     PCNT_CHANNEL_EDGE_ACTION_HOLD = 0, PCNT_CHANNEL_LEVEL_ACTION_KEEP = 0;
struct pcnt_unit_config_t {
    int clk_src = 0, low_limit = 0, high_limit = 0;
    struct {
        bool accum_count = false;
    } flags;
};
struct pcnt_chan_config_t {
    int edge_gpio_num = -1, level_gpio_num = -1;
};
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *, pcnt_unit_handle_t *unit);
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *,
                           pcnt_channel_handle_t *channel);
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t, int, int);
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t, int, int);
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t, const void *);
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t, int);
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t);
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_start(pcnt_unit_handle_t);
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *count);
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t);
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t);
esp_err_t pcnt_del_channel(pcnt_channel_handle_t);
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit);
