#pragma once
#include "reset_plan.h"
#include <atomic>
#include <esp_err.h>
namespace wifichat::wifi {
// Boot-lifetime erase worker. Caller proves Core/Host stopped and API quiescence;
// this worker never receives a Core pointer, settings, key, or a replayable command.
class ResetWorker {
  public:
    enum class State { idle, erasing, done, failed };
    esp_err_t start_after_shutdown();
    State state() const { return state_.load(std::memory_order_acquire); }
    uint32_t percent() const {
        return erased_.load(std::memory_order_relaxed) * 100 / ResetPlan::bytes;
    }
    esp_err_t error() const { return error_; } // Read only after observing failed.

  private:
    static void run(void *);
    std::atomic<State> state_{State::idle};
    std::atomic<uint32_t> erased_{0};
    esp_err_t error_ = ESP_OK;
};
} // namespace wifichat::wifi
