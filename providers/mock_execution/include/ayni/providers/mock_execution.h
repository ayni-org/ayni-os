// Deterministic local execution component; public ABI only, no real hardware or I/O.
#ifndef AYNI_PROVIDERS_MOCK_EXECUTION_H
#define AYNI_PROVIDERS_MOCK_EXECUTION_H
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <memory>
namespace ayni::providers::mock_execution {
enum class StopConfirmation { automatic, manual };
struct Config {
    void *clock_context = nullptr;        // Borrowed until close.
    uint64_t (*now_us)(void *) = nullptr; // Required trusted monotonic clock.
    uint64_t executor_boot_epoch = 1;     // Runtime supplies fresh identity for each gate lifetime.
    uint32_t ticks_to_complete = 8;       // Positive number of accepted samples before success.
    uint32_t call_timeout_ms = 30000; // Positive declared Provider deadline; independent of lease.
    StopConfirmation stop_confirmation = StopConfirmation::automatic;
    ayni_control_envelope_v1 limits{}; // Full measured local bounds, copied.
};
// One owner-thread executor, one action borrow and one independently controlled gate.
// Runtime owns this component; neither Provider callbacks nor advance allocate.
class Executor {
  public:
    Executor();
    ~Executor(); // Retains live context on caller misuse; close reports explicit BUSY.
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    ayni_status_t configure(const Config &config); // One bounded startup allocation.
    ayni_status_t describe_provider(ayni_provider_v1 &out) const;
    ayni_status_t describe_executor(ayni_executor_v1 &out) const;
    ayni_status_t advance();         // One sample/completion and optional local stop measurement.
    ayni_status_t confirm_stopped(); // Explicit simulated measurement, only after STOPPING.
    ayni_status_t info(ayni_control_info_v1 &out) const; // IDLE until first challenge creates gate.
    uint32_t pending() const;      // Provider borrow count, independent of physical stop.
    uint64_t samples() const;      // Accepted local outputs, never increments without permit.
    uint64_t renewals() const;     // Successful gate renewals.
    uint64_t submissions() const;  // Accepted original operations.
    uint64_t logical_call() const; // Last submitted stable call identity, zero before first.
    ayni_status_t close();         // Only after Kernel destruction and measured local stop.
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_; // Stable callback context; startup-only allocation.
};
} // namespace ayni::providers::mock_execution
#endif
