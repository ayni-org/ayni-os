#pragma once
#include "motor_output.h"
#include "motor_wire.h"
#include "transport.h"
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <optional>
namespace usbchat {
// One Core-controlled physical procedure and its volatile operator confirmation.
// All methods run on the owner thread; Transport only supplies a separate stop level.
class MotorExecution {
  public:
    explicit MotorExecution(Transport &transport) : transport_(transport) {}
    ayni_status_t initialize();
    ayni_executor_v1 ports();
    ayni_submit_result_t submit(const ayni_provider_request_v1 *, ayni_provider_completion_t *);
    ayni_submit_result_t cancel(ayni_attempt_ref_v1);
    ayni_status_t advance();
    void receive(Kind, uint64_t, const uint8_t *, size_t);
    void set_accepting(bool accepting) { accepting_ = accepting; }
    void request_stop();
    void request_local_stop() { local_stop_ = true; }
    bool scene_enabled() const;
    int describe(char *out, size_t capacity) const;
    bool close();

  private:
    enum class Mode { disarmed, armed, latched, closed };
    enum class Phase { review_unsent, review, approved, running, observe_unsent, observe, result };
    enum class Effect { none, possible, complete };
    struct Pending {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        ayni_execution_permit_v1 permit{};
        MotorIdentity identity{};
        MotorCommand command{};
        uint64_t review_deadline_us = 0;
        Phase phase = Phase::review_unsent;
        Effect effect = Effect::none;
        bool operator_quiet = false;
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
    bool notify();
    void publish_terminal();
    Transport &transport_;
    MotorOutput *output_ = nullptr; // Internal RAM; survives every RMT transmission.
    void *gate_memory_ = nullptr;
    size_t gate_bytes_ = 0;
    ayni_control_gate_t *gate_ = nullptr;
    ayni_permit_epochs_v1 epochs_{};
    uint64_t executor_boot_ = 0, current_permit_ = 0, refused_through_ = 0;
    uint64_t sequence_ = 0, review_nonce_ = 0, heartbeat_us_ = 0, terminal_notice_ = 0;
    Mode mode_ = Mode::disarmed;
    bool accepting_ = false, local_stop_ = false;
    uint32_t limit_ = motor_percent_max;
    std::optional<Pending> pending_;
    std::array<std::optional<PortRequest>, 4> requests_{};
};
} // namespace usbchat
