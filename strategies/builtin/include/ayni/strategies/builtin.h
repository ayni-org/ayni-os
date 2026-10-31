// Bounded policies and conversation values (ADR-0056). No I/O beyond an injected nonblocking
// clock (ADR-0073), no Kernel internals and no allocation. Objects and borrowed configuration
// live through Kernel destroy; one owner thread uses each strategy. Scratch is reset on every
// decide.
#ifndef AYNI_STRATEGIES_BUILTIN_H
#define AYNI_STRATEGIES_BUILTIN_H
#include <array>
#include <ayni/calendar.h>
#include <ayni/calls.h>
#include <ayni/directory.h>
#include <ayni/workspace.h>
#include <optional>

namespace ayni::strategies {
inline constexpr uint32_t rule_count_max = 8;
inline constexpr uint32_t rule_steps_max = 16;
inline constexpr uint32_t retained_rounds_max = 4; // Independently retained user inputs.
inline constexpr uint32_t turn_text_max = 384;
inline constexpr uint32_t summary_text_max = 512;
inline constexpr uint32_t history_bytes_max = 49152; // Complete groups retained through compaction.
inline constexpr uint32_t state_bytes_max = 65536;   // Portable record/controller upper bound.
inline constexpr uint32_t prompt_bytes_max =
    32768; // Hard framed request bound, independent of budget.
inline constexpr uint32_t context_trigger_percent = 75;
inline constexpr uint32_t context_target_percent = 50;

// Fixed text, size bytes used, no terminator; empty is a valid answer.
template <uint32_t Capacity> struct Text {
    std::array<char, Capacity> bytes{};
    uint32_t size = 0;
    ayni_string_v1 view() const { return {bytes.data(), size}; }
};
// Portable encoded parameter/checkpoint/record; never raw C++ object bytes.
struct Encoded {
    std::array<uint8_t, state_bytes_max> bytes{};
    uint32_t size = 0;
    ayni_bytes_v1 view() const { return {bytes.data(), size}; }
};
// One user turn and its final answer; tools remain in the Task call ledger.
struct Round {
    uint64_t number = 0;
    Text<turn_text_max> user;
    Text<turn_text_max> answer;
};
// Caller-owned conversation, isolated by id/owner/generation. Summary is model
// inference for this conversation only, with inclusive source turn numbers.
struct ConversationRecord {
    uint64_t id = 0;
    uint64_t owner_instance_id = 0;
    uint64_t generation = 1;
    uint64_t revision = 0;
    uint64_t last_round = 0;
    uint64_t summary_first = 0;
    uint64_t summary_last = 0;
    Text<summary_text_max> summary;
    Text<history_bytes_max>
        history;              // Complete standard messages, without fixed system instructions.
    uint64_t history_ref = 0; // Immutable external view; nonzero excludes history from the codec.
    ayni_model_usage_v1 last_usage{}; // Last serving-model observation; NONE after context reset.
    uint64_t last_request_characters = 0; // Unicode characters in the last sent messages/tools.
    uint64_t last_input_estimate =
        0; // Estimated input tokens for that request; never service usage.
    ayni_capability_context_v1
        disclosure{};             // Conservative retained metadata visibility, zero before use.
    uint64_t context_version = 1; // Positive generation; changes when derived context is rebuilt.
    // Independent accepted input survives invalidation before publication.
    Text<turn_text_max> draft;
    uint32_t round_count = 0;
    std::array<Round, retained_rounds_max> rounds{};
};

// An outer store persists the candidate *before* accepting it. begin/finish are
// pure: failure leaves the old record intact. erase increments generation so
// a late completed Task cannot resurrect deleted context. File storage is P2;
// Kernel context intents own the cross-store deletion barrier (ADR-0060).
class Conversation {
  public:
    // Since 0.53: every synchronous codec call borrows a queried exclusive region.
    static ayni_status_t workspace_requirements(size_t *bytes, size_t *alignment);
    static ayni_status_t begin(const ayni_workspace_v1 *workspace, const ConversationRecord &record,
                               ayni_string_v1 user, Encoded &parameters, uint64_t memory_id = 0);
    // Extract independent user input from a validated turn, without exposing the wire layout.
    static ayni_status_t input(const ayni_workspace_v1 *workspace, ayni_bytes_v1 parameters,
                               Text<turn_text_max> &out);
    static ayni_status_t finish(const ayni_workspace_v1 *workspace,
                                const ConversationRecord &current, ayni_bytes_v1 checkpoint,
                                ConversationRecord &candidate);
    // Preserve a superseded publication under its original turn identity, without its answer.
    static ayni_status_t retain_published_input(const ayni_workspace_v1 *workspace,
                                                const ConversationRecord &, uint64_t,
                                                ConversationRecord &);
    static ayni_status_t erase(const ayni_workspace_v1 *workspace,
                               const ConversationRecord &current, ConversationRecord &candidate);
    // Single/historical turn deletion clears generated text and preserves other retained users.
    static ayni_status_t erase_round(const ayni_workspace_v1 *workspace, const ConversationRecord &,
                                     uint64_t, ConversationRecord &);
    static ayni_status_t erase_derived(const ayni_workspace_v1 *workspace,
                                       const ConversationRecord &, ConversationRecord &);
    static ayni_status_t encode(const ayni_workspace_v1 *workspace,
                                const ConversationRecord &record, Encoded &out);
    static ayni_status_t decode(const ayni_workspace_v1 *workspace, ayni_bytes_v1 bytes,
                                ConversationRecord &out);
    // GoalType validator context is a borrowed ayni_workspace_v1 descriptor.
    // Use a wrapper when other GoalType callbacks require a different context.
    static ayni_status_t validate_turn(void *, ayni_bytes_v1 parameters);
};

// Immutable concrete call; all strings/fields borrow the trusted configuration.
struct Call {
    ayni_policy_intent_kind_t kind = AYNI_POLICY_REQUEST_CALL;
    ayni_string_v1 binding_id{};
    uint32_t binding_version = 1;
    ayni_scope_v1 arguments{sizeof(ayni_scope_v1), 0, nullptr};
    ayni_string_v1 text{};
    std::optional<ayni_value_v1>
        input{}; // Explicit complete native input; excludes scalar/text templates.
    uint32_t mode =
        AYNI_CALL_MODE_FOREGROUND; // Explicit managed start; normal calls retain terminal waiting.
};
// Stable name plus semantic version becomes name.vN, at most 32 bytes.
struct RuleStep {
    ayni_string_v1 name{};
    uint32_t version = 1;
    Call call;
};
// Select exactly by GoalType id/version; no expression language or hidden input.
struct Rule {
    ayni_string_v1 goal_type_id{};
    uint32_t goal_type_version = 1;
    const RuleStep *steps = nullptr;
    uint32_t step_count = 0;
};
// Registration identity also names the private checkpoint writer. Readers are
// explicit migration promises, copied by Core; default accepts exact self only.
struct Identity {
    ayni_string_v1 id{};
    uint32_t version = 1;
    const ayni_checkpoint_reader_v1 *readers = nullptr;
    uint32_t reader_count = 0;
};
class Rules {
  public:
    ayni_status_t configure(Identity identity, const Rule *rules, uint32_t count);
    ayni_strategy_v1 describe();

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    Identity identity_{};
    const Rule *rules_ = nullptr;
    uint32_t count_ = 0;
};

// Optional wall clock for model requests (ADR-0073), the one input read outside the snapshot.
// The instructions of each main model request then state the local date, weekday and time in
// timezone, and the zone itself, so the model resolves relative dates; an untrusted sample is
// marked unsynchronized, and without a usable sample or a zone the time is stated as unknown.
// With neither port nor zone nothing is stated. The time changes only that request's text: the
// intent, step key and checkpoint never depend on it (C-POLICY-006).
// utc_now must not block. timezone is a POSIX TZ rule, borrowed like the rest of the config.
struct LocalClock {
    void *context = nullptr;
    void (*utc_now)(void *, ayni_utc_sample_v1 *) = nullptr;
    ayni_string_v1 timezone{};
};
// Both calls are MODEL class; summary binding must have a text result contract.
// model.text and summary.text are trusted instructions, framed apart from the conversation data
// (AYNI_MODEL_INSTRUCTIONS_PREFIX, ADR-0074). User text is in parameters.
// Optional Host-owned long history boundary. Calls copy/return bounded data and
// never perform I/O on the strategy thread. NOT_READY retains no policy output;
// ready becomes true after a worker completes. Views are owner/conversation/generation scoped.
struct ArchivedResult {
    uint64_t reference = 0;  // Immutable backend result version, positive after archive commits.
    uint32_t json_bytes = 0; // Complete rendered UTF-8 bytes; independent of token statistics.
};
struct HistoryPort {
    void *context = nullptr;
    uint32_t (*ready)(void *, uint64_t) = nullptr;
    ayni_status_t (*load)(void *, uint64_t task, const ConversationRecord &,
                          Text<history_bytes_max> &) = nullptr;
    ayni_status_t (*save)(void *, uint64_t task, uint64_t turn, const ConversationRecord &,
                          uint64_t &reference) = nullptr;
    // Complete raw facts/proofs are copied before returning NOT_READY. A successful
    // archive makes immutable rendered chunks available until the next archive.
    ayni_status_t (*archive)(void *, uint64_t task, const ConversationRecord &,
                             const ayni_task_call_result_v1 &, ArchivedResult &) = nullptr;
    ayni_status_t (*result_chunk)(void *, uint64_t task, uint64_t call, uint32_t offset,
                                  Text<prompt_bytes_max> &, uint32_t maximum) = nullptr;
};
struct ToolLoopConfig {
    Identity identity;
    Call model;
    Call summary;
    LocalClock clock;
    Call shell; // Optional trusted exact Shell selection; empty Binding means unavailable.
    uint64_t context_window_tokens =
        0; // Explicit model window; zero disables automatic compression.
    uint64_t output_reserve_tokens = 0; // Positive and below window when compression is enabled.
    HistoryPort history{};
};
class ToolLoop {
  public:
    // Since 0.53: queried scratch is exclusive to this owner through Kernel destruction.
    static ayni_status_t workspace_requirements(size_t *bytes, size_t *alignment);
    ayni_status_t configure(const ToolLoopConfig &config, const ayni_workspace_v1 *workspace);
    ayni_strategy_v1 describe();

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static uint32_t ready(void *, uint64_t);
    ToolLoopConfig config_{};
    bool configured_ = false;
    ayni_workspace_v1 workspace_{}; // Borrowed bytes; present only after successful configure.
};
} // namespace ayni::strategies
#endif
