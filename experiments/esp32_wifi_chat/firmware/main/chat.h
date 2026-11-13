#pragma once
#include "../wifi/model_buffer.h"
#include "model_tools.h"
#include "motor.h"
#include "motor_check.h"
#include "reply_voice.h"
#include "speaker.h"
#include "transport.h"
#include <array>
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/commands.hpp>
#include <ayni/hosts/esp_idf.h>
#include <ayni/hosts/storage/value_admission.h>
#include <ayni/runtime/conversation.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/strategies/builtin.h>
#include <string>
namespace wifichat {
// Experiment composition: one Agent, one model destination fixed per boot, and the built-in
// ToolLoop. Core owns Goals, Tasks, Calls and the ToolLoop schedule; this owns the devices,
// shared conversation ports and the session policy.
class Chat {
  public:
    explicit Chat(Transport &transport)
        : transport_(transport), speaker_(transport), motor_(transport) {}
    // Owner startup before the UI/radios: reserve contiguous control/audio stacks.
    // The audio worker stays idle; motor hardware/worker initialize automatically during
    // composition.
    ayni_status_t reserve_control_memory();
    ayni_status_t initialize(const ModelSelection &selection);
    // Advances the devices, steps Core once and moves the session on. report is that step's
    // report; again is set when this tick changed state the step has not seen yet.
    ayni_status_t tick(ayni_step_report_v1 &report, bool &again);
    void incoming(const Packet &);
    void stop_outputs() {
        reply_voice_.set_accepting(false);
        speaker_.set_accepting(false);
        speaker_.request_stop();
        motor_.execution().set_accepting(false);
        motor_.execution().request_stop();
    }

  private:
    // The ToolLoop, held back until the session policy announced readiness: restored work
    // waits for its cancellation instead of proposing model or device calls.
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static uint32_t admit(void *, uint64_t);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    static ayni_submit_result_t submit_model(void *, const ayni_provider_request_v1 *,
                                             ayni_provider_completion_t *);
    static ayni_submit_result_t cancel_model(void *, ayni_attempt_ref_v1);
    ayni_status_t configure();
    ayni_status_t configure_model();
    ayni_status_t configure_strategy();
    ayni_status_t configure_motor_check();
    ayni_status_t command(ayni_intent_kind_t, const void *, uint64_t * = nullptr, uint32_t = 0,
                          ayni_reason_t * = nullptr);
    ayni_status_t receipt();
    ayni_status_t settle_session(uint64_t durable_seq, bool &settled);
    ayni_status_t prepare(const ayni_kernel_info_v1 &);
    ayni_status_t prepare_owner(const ayni_kernel_info_v1 &);
    ayni_status_t prepare_grant();
    ayni_status_t prepare_history(const ayni_kernel_info_v1 &);
    ayni_status_t close_session(const ayni_kernel_info_v1 &);
    ayni_status_t observe_probe(const ayni_kernel_info_v1 &);
    ayni_status_t converse(const ayni_kernel_info_v1 &);
    ayni_status_t present_reply();
    void finish_voice(ayni_status_t);
    void end_turn(const char *);
    void wait_for(const char *phase, const char *label, ayni_status_t);
    void status(uint64_t, const char *);
    void publish_model();
    void publish_proposals();      // Best-effort UI metadata, never execution approval.
    void trace(const char *phase); // Fixed phase/timing metadata only.
    void trace_latency(const char *phase);
    void diagnostics();
    Transport &transport_;
    Speaker speaker_;
    Motor motor_;
    MotorCheck motor_check_; // Local manual checks share Core/PermitGate, without model calls.
    ReplyVoice reply_voice_;
    bool voice_reply_enabled_ = false; // UI-selected automatic presentation only, not tool access.
    struct VoiceNotice {
        uint64_t id;
        uint8_t enabled;
    };
    ayni_status_t reported_voice_failure_ = AYNI_OK; // Last displayed drain failure, per turn.
    std::optional<VoiceNotice> voice_notice_;        // Matching acknowledgement waits for UI space.
    ayni::hosts::esp_idf::Host *host_ = nullptr;     // Internal RAM, outlives Kernel/producers.
    ayni_kernel_t *kernel_ = nullptr;
    void *arena_ = nullptr;
    // Every conversation text and model request body, on the Host's value files (ADR-0096).
    static constexpr uint32_t host_capacity = 16, value_capacity = 8;
    ayni::hosts::storage::ValueAdmission<value_capacity>
        values_; // Reserves shared Host queue room for Core log/maintenance requests.
    ayni::runtime::FileBodies bodies_;
    ayni::strategies::ToolLoop loop_;
    ayni_strategy_v1 inner_{}, strategy_{};
    // One owner-only region for the ToolLoop and conversation codecs; never used inside a
    // nested callback of theirs.
    ayni_workspace_v1 workspace_{};
    ayni::runtime::conversation::Session history_;
    uint64_t rounds() const {
        const auto *record = history_.record();
        return record ? record->last_round : 0;
    }
    bool history_configured_ = false;
    uint64_t session_ = 0, instance_ = 0;
    ayni::CommandClient commands_;
    uint64_t grant_ = 0, goal_ = 0, turn_ = 0;
    bool announced_ = false, hold_reported_ = false, cancel_requested_ = false;
    bool progress_ = false; // A command or state change this tick that Core has not stepped.
    enum class ReplyIntent { present, cancelled };
    ReplyIntent reply_intent_ = ReplyIntent::present; // Independent of Core Goal cancellation.
    // One turn at a time: its text until the Goal exists, then the Goal until its answer.
    enum class Turn { idle, preparing_input, input, running, saving, answer, speaking, reply_done };
    Turn turn_state_ = Turn::idle;
    struct ProbeTurn {
        uint32_t characters = 0; // Zero only primes the transport template; otherwise Han text.
        std::unique_ptr<wifi::ModelBuffer> history;
        ayni_body_ref_v1 history_ref{};
        uint32_t history_bytes = 0;
    };
    std::optional<ProbeTurn> probe_; // Present only for an isolated diagnostic Goal.
    ayni_status_t probe_input(ayni_string_v1, ayni::strategies::Encoded &);
    std::string input_, answer_;
    std::array<char, 192> terminal_text_{}; // Owned error text until the terminal queue accepts it.
    const char *reply_end_ = nullptr; // Static or terminal_text_, valid throughout reply_done.
    struct PreparationWait {
        const char *phase = nullptr; // None, or an internal static phase name (never user text).
        uint64_t next_trace_us = 0;  // Monotonic microseconds, only valid with phase.
    } preparation_wait_;
    uint64_t turn_started_ = 0;
    // Owner-only per-turn wall time. Step/conversation work may overlap storage on its worker;
    // storage snapshots are cumulative completed work, sampled as deltas at phase boundaries.
    struct TurnTiming {
        uint64_t step_calls = 0, step_us = 0, max_step_us = 0, conversation_us = 0;
        ayni::hosts::esp_idf::StorageMetrics storage;
    } timing_;
    uint64_t reply_deadline_ = 0; // Deadline for final-text queue admission only.
    uint32_t model_requests_ = 0; // Metadata only, includes any context-compaction calls.
    ayni::strategies::Encoded parameters_{}; // Goal parameters, then the final checkpoint.
    enum class Forget { none, requested, saving };
    Forget forget_ = Forget::none;
    // Normal close cancels experiment Goals; confirmed rebuild quiesces API
    // borrows first, then uses public shutdown without needing spare Goal slots.
    // stopped means Kernel/resources are gone and the terminal report is waiting
    // for transport capacity; it is never announced before actual teardown.
    enum class Closing { open, quiescing, cancel_work, drain_session, kernel, stopped };
    Closing closing_ = Closing::open;
    ModelSelection selection_{};
    ayni::providers::model_protocol::RequestOptions protocol_options_{};
    char route_[32]{};
    // The model service: tool-calling turns and text-only summaries, one slot, one destination.
    ayni_call_resource_v1 resource_{};
    std::array<ayni_schema_node_v1, 3> input_schema_{}, large_input_schema_{};
    // Index 0: tool-calling turns (answer_bytes of text); 1: summaries (summary_bytes).
    std::array<ayni_schema_node_v1, 4> result_schema_{};
    ayni_authority_projection_v1 projection_{};
    std::array<ayni_business_contract_v1, 4> business_{};
    std::array<ayni_call_capability_v1, 4> capabilities_{};
    ayni_provider_v1 provider_{};
    ayni_string_v1 destination_{};
    ayni_scope_field_v1 destination_field_{};
    ayni_scope_v1 destination_scope_{};
    ayni_provider_completion_t *completion_ = nullptr;
    ayni_attempt_ref_v1 attempt_{};
    uint64_t call_ = 0, deadline_ = 0;
    bool summary_ = false; // The pending request asked for text only.
    enum class ModelState { idle, waiting, text, tools, failed, cancelled };
    ModelState model_ = ModelState::idle;
    std::string model_answer_; // Complete selected reply encoding, stored before publication.
    uint32_t model_tool_count_ = 0;
    ayni_model_usage_v1 model_usage_{};
    ayni_provider_result_v1 model_failure_{}; // Explicit adapter refusal, otherwise UNKNOWN.
    ModelTools model_tools_;
    std::array<char, 1024> diagnostics_{}; // Metadata-only capacity/status rendering.
    ayni_provider_completion_t *screen_completion_ = nullptr;
    ayni_attempt_ref_v1 screen_attempt_{};
    uint64_t screen_call_ = 0, screen_deadline_ = 0;
    uint32_t screen_pages_ = 0;
    enum class ScreenState { idle, waiting, painted, failed };
    ScreenState screen_ = ScreenState::idle;
    ayni_schema_node_v1 screen_input_[2]{}, screen_result_{};
    ayni_business_contract_v1 screen_business_{};
    ayni_call_resource_v1 screen_resource_{};
    ayni_call_capability_v1 screen_capability_{};
    ayni_provider_v1 screen_provider_{};
    ayni_status_t configure_screen();
    static ayni_submit_result_t submit_screen(void *, const ayni_provider_request_v1 *,
                                              ayni_provider_completion_t *);
    static ayni_submit_result_t cancel_screen(void *, ayni_attempt_ref_v1);
    void publish_screen();
};
// Largest canonical inputs of the contracts this composition registers.
uint32_t model_input_bytes(ayni_string_v1 destination);
uint32_t screen_input_bytes();
} // namespace wifichat
