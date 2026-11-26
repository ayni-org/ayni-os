#pragma once
#include <cstddef>
#include <driver/rmt_encoder.h>
struct TestTxChannel;
struct TestSyncManager;
using rmt_channel_handle_t = TestTxChannel *;
using rmt_sync_manager_handle_t = TestSyncManager *;
inline constexpr int RMT_CLK_SRC_DEFAULT = 0;
struct rmt_tx_channel_config_t {
    int gpio_num = -1, clk_src = 0;
    uint32_t resolution_hz = 0;
    size_t mem_block_symbols = 0, trans_queue_depth = 0;
};
struct rmt_sync_manager_config_t {
    rmt_channel_handle_t *tx_channel_array;
    size_t array_size;
};
struct rmt_transmit_config_t {
    int loop_count = 0;
    struct {
        uint32_t eot_level = 0;
        bool queue_nonblocking = false;
    } flags;
};
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *channel);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
esp_err_t rmt_del_channel(rmt_channel_handle_t channel);
esp_err_t rmt_new_sync_manager(const rmt_sync_manager_config_t *, rmt_sync_manager_handle_t *sync);
esp_err_t rmt_del_sync_manager(rmt_sync_manager_handle_t sync);
esp_err_t rmt_sync_reset(rmt_sync_manager_handle_t sync);
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t, const void *payload,
                       size_t bytes, const rmt_transmit_config_t *config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms);
