#pragma once
#include "history.h"
#include "model_tools.h"
#include "motor.h"
#include "speaker.h"
#include "transport.h"
#include "value_queue.h"
#include <array>
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/hosts/esp_idf.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/strategies/builtin.h>
#include <string>
namespace wifichat {
// Experiment composition: one Agent, one model destination fixed per boot, and the built-in
// ToolLoop. Core owns Goals, Tasks, Calls and the ToolLoop schedule; this owns the devices,
// the conversation record (History) and the session policy.
class Chat {
  public:
    explicit Chat(Transport &transport)
        : transport_(transport), speaker_(transport), motor_(transport) {}
    ayni_status_t initialize(const char *route);
    // Advances the devices, steps Core once and moves the session on. report is that step's
    // report; again is set when this tick changed state the step has not seen yet.
    ayni_status_t tick(ayni_step_report_v1 &report, bool &again);
    void incoming(const Packet &);
    void stop_outputs() {
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
    ayni_status_t command(ayni_intent_kind_t, const void *, uint64_t * = nullptr, uint32_t = 0);
    ayni_status_t receipt();
    ayni_status_t settle_session(uint64_t durable_seq, bool &settled);
    ayni_status_t prepare(const ayni_kernel_info_v1 &);
    ayni_status_t converse(const ayni_kernel_info_v1 &);
    ayni_status_t present_reply();
    void end_turn(const char *);
    void status(uint64_t, const char *);
    void publish_model();
    void publish_proposals();            // Best-effort UI metadata, never execution approval.
    void trace(const char *phase) const; // Fixed phase/timing metadata only.
    void diagnostics();
    Transport &transport_;
    Speaker speaker_;
    Motor motor_;
    ayni::hosts::esp_idf::Host *host_ = nullptr; // Internal RAM, outlives Kernel/producers.
    ayni_kernel_t *kernel_ = nullptr;
    void *arena_ = nullptr;
    // Every conversation text and model request body, on the Host's value files (ADR-0096).
    ValueQueue values_; // Reserves shared Host queue room for Core log/maintenance requests.
    BodyCache bodies_;
    ayni::strategies::ToolLoop loop_;
    ayni_strategy_v1 inner_{}, strategy_{};
    // One owner-only region for the ToolLoop and conversation codecs; never used inside a
    // nested callback of theirs.
    ayni_workspace_v1 workspace_{};
    History history_;
    bool history_configured_ = false;
    uint64_t session_ = 0, instance_ = 0, boot_ = 0, sequence_ = 0, pending_command_ = 0;
    uint64_t grant_ = 0, goal_ = 0, turn_ = 0;
    bool announced_ = false, hold_reported_ = false, cancel_requested_ = false;
    bool progress_ = false; // A command or state change this tick that Core has not stepped.
    enum class ReplyIntent { present, cancelled };
    ReplyIntent reply_intent_ = ReplyIntent::present; // Independent of Core Goal cancellation.
    // One turn at a time: its text until the Goal exists, then the Goal until its answer.
    enum class Turn { idle, preparing_input, input, running, saving, answer, reply_done };
    Turn turn_state_ = Turn::idle;
    std::string input_, answer_;
    const char *reply_end_ = nullptr; // Static status text, valid only in reply_done.
    uint64_t turn_started_ = 0;
    uint64_t reply_deadline_ = 0; // Deadline for final-text queue admission only.
    uint32_t model_requests_ = 0; // Metadata only, includes any context-compaction calls.
    ayni::strategies::Encoded parameters_{}; // Goal parameters, then the final checkpoint.
    enum class Forget { none, requested, saving };
    Forget forget_ = Forget::none;
    // Session teardown cancels experiment Goals before Kernel/Host shutdown.
    // stopped means Kernel/resources are gone and the terminal report is waiting
    // for transport capacity; it is never announced before actual teardown.
    enum class Closing { open, cancel_work, kernel, stopped };
    Closing closing_ = Closing::open;
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
    std::array<char, summary_bytes> model_text_{};
    size_t model_text_size_ = 0;
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
