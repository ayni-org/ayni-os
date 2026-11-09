#pragma once
#include "motor_output.h"
#include "motor_plan.h"
#include "transport.h"
#include <atomic>
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <freertos/task.h>
#include <optional>
namespace wifichat {
// One Core-controlled physical procedure after explicit boot-local preparation.
// Core callbacks only copy bounded inbox messages. The control worker alone owns
// Gate/GPIO state; completions use the public any-thread mailbox APIs.
class MotorExecution {
  public:
    explicit MotorExecution(Transport &transport) : transport_(transport) {}
    // Owner-only before Host startup fragments internal RAM; does not acquire hardware.
    ayni_status_t reserve_control_stack();
    ayni_status_t initialize();
    ayni_executor_v1 ports();
    ayni_submit_result_t submit(const ayni_provider_request_v1 *, ayni_provider_completion_t *);
    ayni_submit_result_t cancel(ayni_attempt_ref_v1);
    ayni_status_t advance();
    void receive(Kind, uint64_t, const uint8_t *, size_t);
    void set_accepting(bool accepting) { accepting_.store(accepting, std::memory_order_release); }
    void request_stop();
    void request_local_stop() { request_stop(); }
    int describe(char *out, size_t capacity) const;
    bool close();

  private:
    enum class Mode { disarmed, armed, latched, closed };
    enum class Phase { prepared, running, result };
    enum class Effect { none, possible, partial, complete };
    struct Pending {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        ayni_execution_permit_v1 permit{};
        MotorCommand command{};
        uint64_t last_sample_us = 0; // Local capture time, telemetry only; zero before sample.
        Phase phase = Phase::prepared;
        Effect effect = Effect::none;
        ayni_call_outcome_t outcome = AYNI_CALL_COMPLETED;
        ayni_status_t status = AYNI_OK;
    };
    struct PortRequest {
        ayni_executor_request_t kind = 0;
        ayni_attempt_ref_v1 identity{};
        ayni_executor_completion_t *completion = nullptr;
        ayni_permit_epochs_v1 epochs{};
        uint64_t permit_id = 0;
    };
    enum class MessageKind { start, cancel, port, frame };
    struct Message {
        MessageKind kind = MessageKind::frame;
        Pending start{};
        ayni_attempt_ref_v1 cancel{};
        PortRequest port{};
        Kind frame_kind{};
        uint64_t frame_id = 0;
        uint32_t frame_size = 0;
        std::array<uint8_t, sizeof(uint32_t)> frame{};
    };
    static constexpr size_t inbox_slots = 8;
    static constexpr uint32_t worker_stack_bytes = 24576, worker_priority = 7;
    static constexpr uint32_t snapshot_limit_mask = 0xff, snapshot_mode_shift = 8,
                              snapshot_mode_mask = 0xf, snapshot_busy = 1u << 12;
    ayni_status_t initialize_owner();
    bool send(const Message &);
    void begin(Pending);
    void reject(const Pending &, ayni_status_t);
    static void worker_entry(void *);
    void run();
    void drain_inbox();
    void publish_snapshot();
    void release_worker_storage();
    ayni_status_t advance_owner();
    void receive_owner(Kind, uint64_t, const uint8_t *, size_t);
    void stop_owner();
    bool close_owner();
    static ayni_status_t challenge_port(void *, ayni_attempt_ref_v1, const ayni_permit_epochs_v1 *,
                                        ayni_executor_completion_t *);
    static ayni_status_t renew_port(void *, ayni_attempt_ref_v1, const ayni_permit_renewal_v1 *,
                                    ayni_executor_completion_t *);
    static ayni_status_t stop_port(void *, ayni_attempt_ref_v1, uint64_t,
                                   ayni_executor_completion_t *);
    static ayni_status_t status_port(void *, ayni_attempt_ref_v1, uint64_t,
                                     ayni_executor_completion_t *);
    void trace(const char *, ayni_status_t);
    ayni_status_t queue(const PortRequest &);
    ayni_status_t serve(PortRequest &);
    ayni_status_t challenge(const ayni_permit_epochs_v1 &, ayni_executor_nonce_v1 &);
    ayni_status_t status(uint64_t, ayni_executor_status_v1 &);
    ayni_control_observation_v1 live() const;
    ayni_status_t stop_output();
    ayni_status_t post_result();
    ayni_status_t sample();
    ayni_status_t advance_pending();
    void end(ayni_call_outcome_t, ayni_status_t);
    void publish_terminal();
    Transport &transport_;
    MotorOutput *output_ = nullptr; // Internal RAM; survives every RMT transmission.
    void *gate_memory_ = nullptr;
    size_t gate_bytes_ = 0;
    ayni_control_gate_t *gate_ = nullptr;
    ayni_permit_epochs_v1 epochs_{};
    uint64_t executor_boot_ = 0, current_permit_ = 0, refused_through_ = 0;
    uint64_t sequence_ = 0, terminal_notice_ = 0;
    Mode mode_ = Mode::disarmed;
    // Owner-only, one hardware acquisition attempt per boot. A partial cleanup
    // failure must retain its objects and cannot be overwritten by another Prepare.
    enum class Initialization { unattempted, failed, ready };
    Initialization initialization_ = Initialization::unattempted;
    // Independent latest-level interlocks bypass the command inbox.
    std::atomic<bool> accepting_{false}, local_stop_{false}, inbox_open_{false}, closing_{false},
        worker_stopped_{false};
    std::atomic<uint64_t> heartbeat_capture_{0};
    std::atomic<ayni_status_t> worker_error_{AYNI_OK};
    std::atomic<uint32_t> snapshot_{motor_percent_max};
    QueueHandle_t inbox_ = nullptr;
    StaticQueue_t *inbox_tcb_ = nullptr;
    uint8_t *inbox_storage_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    StaticTask_t *worker_tcb_ = nullptr;
    StackType_t *worker_stack_ = nullptr;
    uint64_t last_cycle_us_ = 0, cycle_gap_max_us_ = 0;
    uint32_t limit_ = motor_percent_max;
    std::optional<Pending> pending_;
    std::array<std::optional<PortRequest>, 4> requests_{};
};
} // namespace wifichat
