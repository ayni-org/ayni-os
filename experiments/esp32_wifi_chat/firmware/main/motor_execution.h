#pragma once
#include "motor_admission.h"
#include "motor_output.h"
#include "motor_plan.h"
#include "output_intervals.h"
#include "transport.h"
#include <atomic>
#include <ayni/calls.h>
#include <ayni/control_session.h>
#include <ayni/execution.h>
#include <freertos/task.h>
#include <optional>
namespace wifichat {
// One Core-controlled physical procedure while the local enable interlock is open.
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
    // Async recoverable soft-stop tool path: confirms GPIO low readback and keeps Mode::armed
    // unless stop fails, in which case it auto-escalates to Mode::latched (ADR-0111).
    ayni_submit_result_t submit_soft_stop(const ayni_provider_request_v1 *,
                                          ayni_provider_completion_t *);
    ayni_submit_result_t cancel(ayni_attempt_ref_v1);
    ayni_status_t advance();
    uint64_t latest_dispatch() const { return latest_dispatch_; } // Owner thread only.
    // Owner reads the control worker's fixed-size output intervals for a model window.
    bool output_intervals(uint64_t from_us, uint64_t to_us, OutputIntervals &);
    void receive(Kind, uint64_t, const uint8_t *, size_t);
    void set_accepting(bool accepting) { accepting_.store(accepting, std::memory_order_release); }
    void request_stop();      // Latched hard E-stop (Mode::latched until reboot).
    void request_soft_stop(); // Recoverable soft-stop (stays Mode::armed unless stop fails).
    void wake_control();      // Best-effort inbox nudge; the stop latch is separate.
    int describe(char *out, size_t capacity) const;
    bool close();

  private:
    uint64_t latest_dispatch_ = 0; // Last Provider acceptance on the Core owner.
    using Mode = ControlMode;
    enum class Phase { prepared, running, result };
    enum class Effect { none, possible, partial, complete };
    struct Pending {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        ayni_execution_permit_v1 permit{};
        MotorTrajectory trajectory{};
        uint32_t segment_index = 0;      // Active segment index in trajectory.
        uint32_t segments_completed = 0; // Hardware-verified completed segments.
        uint64_t call = 0;               // Core Call identity, used only for timing correlation.
        uint64_t last_sample_us = 0;     // Local capture time, telemetry only; zero before sample.
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
        uint64_t echoed_nonce = 0; // Fresh renewal nonce for AYNI_EXECUTOR_RENEW (ADR-0112).
    };
    struct SoftStopRequest {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
    };
    enum class MessageKind { start, cancel, port, frame, soft_stop, wake };
    struct Message {
        MessageKind kind = MessageKind::frame;
        Pending start{};
        ayni_attempt_ref_v1 cancel{};
        PortRequest port{};
        SoftStopRequest soft_stop{};
        Kind frame_kind{};
        uint64_t frame_id = 0;
        uint32_t frame_size = 0;
        std::array<uint8_t, sizeof(uint32_t)> frame{};
    };
    static constexpr size_t inbox_slots = 8;
    static constexpr uint32_t worker_stack_bytes = 12288, worker_priority = 7;
    static constexpr uint32_t snapshot_limit_mask = 0xff, snapshot_mode_shift = 8,
                              snapshot_mode_mask = 0xf, snapshot_busy = 1u << 12;
    ayni_status_t initialize_owner();
    bool send(const Message &);
    void begin(const Pending &);
    void reject(const Pending &, ayni_status_t);
    static void worker_entry(void *);
    void run();
    void handle_message(const Message &);
    void drain_inbox(TickType_t wait_ticks);
    void publish_snapshot();
    void release_worker_storage();
    ayni_status_t advance_owner();
    void receive_owner(Kind, uint64_t, const uint8_t *, size_t);
    void stop_owner();
    ayni_status_t soft_stop_owner();
    void serve_soft_stop(const SoftStopRequest &);
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
    void trace_handoff(const char *phase, uint64_t call, uint64_t at_us);
    void mark_output_start(uint64_t call, uint64_t at_us);
    void mark_output_stop(uint64_t call, uint64_t at_us);
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
    void *session_memory_ = nullptr;
    size_t session_bytes_ = 0;
    ayni_control_session_t *session_ = nullptr;
    ayni_permit_epochs_v1 epochs_{};
    uint64_t executor_boot_ = 0, current_permit_ = 0, refused_through_ = 0;
    uint64_t sequence_ = 0, terminal_notice_ = 0;
    Mode mode_ = Mode::disarmed;
    MotorAdmission admission_;
    bool state_notice_ = true; // Worker retries initial/mode snapshots until UI admission.
    bool initialization_reported_ = false; // Owner-only failure notification when no worker exists.
    // Owner-only, one hardware acquisition attempt per boot. A partial cleanup
    // failure must retain its objects and cannot be overwritten by another Prepare.
    enum class Initialization { unattempted, failed, ready };
    Initialization initialization_ = Initialization::unattempted;
    // Independent latest-level interlocks bypass the command inbox.
    std::atomic<bool> accepting_{false}, closing_{false}, worker_stopped_{false};
    // High bit closes producer admission; low bits count borrows until xQueueSend returns.
    static constexpr uint32_t inbox_closed = 1U << 31U;
    std::atomic<uint32_t> inbox_access_{inbox_closed};
    std::atomic<uint64_t> heartbeat_capture_{0};
    std::atomic<ayni_status_t> worker_error_{AYNI_OK};
    std::atomic<uint32_t> snapshot_{motor_percent_max};
    portMUX_TYPE output_mutex_ = portMUX_INITIALIZER_UNLOCKED;
    OutputIntervals output_intervals_{};
    uint64_t output_count_ = 0, last_overwritten_stop_us_ = 0; // Guarded by output_mutex_.
    QueueHandle_t inbox_ = nullptr;
    StaticQueue_t *inbox_tcb_ = nullptr;
    uint8_t *inbox_storage_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    StaticTask_t *worker_tcb_ = nullptr;
    StackType_t *worker_stack_ = nullptr;
    uint64_t last_cycle_us_ = 0, cycle_gap_max_us_ = 0;
    uint32_t limit_ = motor_percent_max;
    std::optional<Pending> pending_;
    // Worker-owned staging copied by session_open. Keep the bounded array out of the task stack.
    std::array<ayni_control_segment_v1, motor_segments_max> session_segments_{};
    std::array<std::optional<PortRequest>, 4> requests_{};
};
} // namespace wifichat
