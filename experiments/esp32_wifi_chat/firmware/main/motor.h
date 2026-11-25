#pragma once
#include "motor_execution.h"
#include "motor_stream.h"
#include <array>
#include <ayni/calls.h>
#include <ayni/runtime/operation_session.h>
namespace wifichat {
// Owns copied tool descriptors and the one local physical executor. Core owns Calls.
class Motor {
  public:
    explicit Motor(Transport &transport) : transport_(transport), execution_(transport) {}
    MotorExecution &execution() { return execution_; }
    const MotorExecution &execution() const { return execution_; }
    ayni_status_t configure(ayni_kernel_t *);
    ayni_status_t configure_session(ayni_kernel_t *, const ayni_workspace_v1 *,
                                    ayni::runtime::OperationSession::Commands);
    ayni_status_t advance_sessions(ayni_kernel_t *, uint64_t boot, uint64_t durable);
    bool owns_session(ayni_string_v1 type, uint32_t version) const {
        return activity_.owns(type, version);
    }
    bool sessions_drained() const {
        return activity_.snapshot().phase == ayni::runtime::OperationSession::Phase::idle &&
               !opening_ && !execute_ && !steering_;
    }
    void cancel_sessions(); // Cancel current work; later independent operations remain allowed.
    void close_sessions();
    // Continues retained Provider completion delivery after Core stops accepting commands.
    ayni_status_t drain_sessions();

  private:
    using Activity = ayni::runtime::OperationSession;
    ayni_status_t advance_opening(ayni_kernel_t *, uint64_t boot);
    ayni_status_t deliver_opening(const Activity::Snapshot &);
    ayni_status_t advance_execute(ayni_kernel_t *);
    ayni_status_t advance_steering(ayni_kernel_t *);
    ayni_status_t settle_operation(const Activity::Snapshot &);
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_status_t session_requirements(void *, ayni_bytes_v1,
                                              const ayni_goal_requirement_sink_v1 *);
    ayni_submit_result_t open_session(const ayni_provider_request_v1 &,
                                      ayni_provider_completion_t *);
    ayni_submit_result_t steer(const ayni_provider_request_v1 &, ayni_provider_completion_t *);
    ayni_status_t complete_steering();
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
    Transport &transport_;
    MotorExecution execution_;
    std::array<ayni_schema_node_v1, 6> run_input_{};
    std::array<ayni_authority_projection_v1, 3> projections_{};
    std::array<ayni_schema_node_v1, stream_nodes> stream_input_{};
    std::array<ayni_schema_node_v1, 5> steer_input_{};
    std::array<ayni_authority_projection_v1, stream_field_count> stream_projections_{};
    std::array<ayni_authority_projection_v1, 3> steer_projections_{};
    ayni_call_resource_v1 resource_{};
    ayni_schema_node_v1 status_input_{}, result_{};
    std::array<ayni_business_contract_v1, 6> business_{};
    std::array<ayni_call_capability_v1, 6> offers_{};
    ayni_provider_v1 provider_{};
    ayni::runtime::OperationSession activity_;
    const ayni_workspace_v1 *session_workspace_ = nullptr; // Borrowed owner-only decode workspace.
    std::array<ayni_value_node_v1, stream_nodes> session_nodes_{};
    struct Opening {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        uint64_t source_call = 0;
        MotorStreamInput input;
        enum class Phase { queued, creating, rollback } phase = Phase::queued;
    };
    struct Execute {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        uint64_t call = 0;
        ayni_execution_permit_v1 permit{};
        MotorCommand initial;
        enum class State { queued, cancelled } state = State::queued;
    };
    struct Steering {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        uint64_t source_call = 0, seq = 0; // seq=0 before mailbox admission.
        ControlTarget target;
        ayni_status_t status = AYNI_OK;
    };
    enum class SessionAdmission { open, closing } session_admission_ = SessionAdmission::open;
    std::optional<Steering> steering_;
    std::optional<Opening> opening_;
    std::optional<Execute> execute_;
    std::optional<MotorStreamInput>
        active_stream_; // Immutable authorized envelope, not GPIO state.
};
} // namespace wifichat
