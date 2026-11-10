#include "reset_worker.h"
#include <algorithm>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace wifichat::wifi {
namespace {
constexpr uint32_t stack_bytes = 4096;
const esp_partition_t *target() {
    const auto *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ayni");
    return partition && ResetPlan::matches(partition->label, partition->address, partition->size)
               ? partition
               : nullptr;
}
} // namespace
esp_err_t ResetWorker::start_after_shutdown() {
    if (state() != State::idle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!target()) {
        error_ = ESP_ERR_NOT_FOUND;
        state_.store(State::failed, std::memory_order_release);
        return error_;
    }
    state_.store(State::erasing, std::memory_order_release);
    if (xTaskCreate(run, "reset-ayni", stack_bytes, this, 2, nullptr) != pdPASS) {
        error_ = ESP_ERR_NO_MEM;
        state_.store(State::failed, std::memory_order_release);
        return error_;
    }
    return ESP_OK;
}
void ResetWorker::run(void *context) {
    auto &self = *static_cast<ResetWorker *>(context);
    const auto *partition = target();
    esp_err_t status = partition ? ESP_OK : ESP_ERR_NOT_FOUND;
    uint32_t erased = 0;
    while (status == ESP_OK && erased < ResetPlan::bytes) {
        const auto count = std::min(ResetPlan::chunk, ResetPlan::bytes - erased);
        status = esp_partition_erase_range(partition, erased, count);
        if (status == ESP_OK) {
            erased += count;
            self.erased_.store(erased, std::memory_order_relaxed);
        }
        vTaskDelay(1);
    }
    self.error_ = status;
    self.state_.store(status == ESP_OK ? State::done : State::failed, std::memory_order_release);
    vTaskDelete(nullptr); // All flash I/O finished before publishing the result.
}
} // namespace wifichat::wifi
