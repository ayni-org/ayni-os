#pragma once
#include "model_tools.h"
#include "motor.h"
#include "speaker.h"
#include "transport.h"
#include <array>
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/hosts/esp_idf.h>
namespace usbchat {
// Experiment composition: one text-only Agent, one model destination fixed per boot.
// All state changes and the completion checkpoint belong to the unchanged Ayni Core.
class Chat {
  public:
    explicit Chat(Transport &transport)
        : transport_(transport), speaker_(transport), motor_(transport) {}
    ayni_status_t initialize(const char *route);
    ayni_status_t tick();
    void incoming(const Packet &);
    void stop_outputs() {
        motor_.execution().set_accepting(false);
        motor_.execution().request_stop();
        motor_.execution().advance();
    }

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static ayni_status_t validate(void *, ayni_bytes_v1);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    static ayni_submit_result_t submit_model(void *, const ayni_provider_request_v1 *,
                                             ayni_provider_completion_t *);
    static ayni_submit_result_t cancel_model(void *, ayni_attempt_ref_v1);
    ayni_status_t configure();
    ayni_status_t command(ayni_intent_kind_t, const void *, uint64_t * = nullptr, uint32_t = 0);
    ayni_status_t receipt();
    ayni_status_t settle_session(uint64_t durable_seq, bool &settled);
    void status(uint64_t, const char *);
    void publish_model();
    void diagnostics();
    Transport &transport_;
    Speaker speaker_;
    Motor motor_;
    ayni::hosts::esp_idf::Host *host_ = nullptr; // Internal RAM, outlives Kernel/producers.
    ayni_kernel_t *kernel_ = nullptr;
    void *arena_ = nullptr;
    uint64_t session_ = 0, instance_ = 0, boot_ = 0, sequence_ = 0, pending_command_ = 0;
    uint64_t grant_ = 0, goal_ = 0, turn_ = 0;
    bool announced_ = false, hold_reported_ = false, cancel_requested_ = false;
    // Session teardown cancels experiment Goals before Kernel/Host shutdown.
    enum class Closing { open, cancel_work, kernel };
    Closing closing_ = Closing::open;
    char route_[32]{};
    ayni_call_resource_v1 resource_{};
    std::array<ayni_schema_node_v1, 3> input_{};
    ayni_schema_node_v1 result_{};
    ayni_authority_projection_v1 projection_{};
    ayni_business_contract_v1 business_{};
    ayni_call_capability_v1 capability_{};
    ayni_provider_v1 provider_{};
    ayni_string_v1 destination_{};
    ayni_provider_completion_t *completion_ = nullptr;
    ayni_attempt_ref_v1 attempt_{};
    uint64_t call_ = 0, deadline_ = 0;
    enum class ModelState { idle, waiting, text, tools, failed, cancelled };
    ModelState model_ = ModelState::idle;
    std::array<char, answer_bytes> answer_{};
    size_t answer_size_ = 0;
    std::array<char, answer_bytes + 16> checkpoint_{};
    ModelTools model_tools_;
    std::array<char, prompt_bytes> policy_messages_{};
    std::array<char, tx_bytes> json_{};
    std::array<char, 8192> arguments_json_{};
    std::array<char, 4096> tool_reply_{};
    // One owner-only public normalization region; no worker or nested callback borrows it.
    ayni_workspace_v1 value_workspace_{};
    std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> input_arguments_{};
    std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> input_values_{};
    std::array<char, 8192> input_text_{};
    ayni_schema_v1 tool_schema(ayni_string_v1 binding) const;
    ayni_status_t reply_to_tool(const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *,
                                const ayni_tool_call_v1 &, ayni_string_v1 step_key,
                                bool &intent_submitted, ayni_string_v1 &reply);
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
} // namespace usbchat
