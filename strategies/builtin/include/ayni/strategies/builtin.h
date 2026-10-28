// Bounded policies and conversation values (ADR-0056). No I/O beyond an injected nonblocking
// clock (ADR-0073), no Kernel internals and no allocation. Objects and borrowed configuration
// live through Kernel destroy; one owner thread uses each strategy. Scratch is reset on every
// decide.
#ifndef AYNI_STRATEGIES_BUILTIN_H
#define AYNI_STRATEGIES_BUILTIN_H
#include <array>
#include <ayni/calendar.h>
#include <ayni/calls.h>

namespace ayni::strategies {
inline constexpr uint32_t rule_count_max = 8;
inline constexpr uint32_t rule_steps_max = 16;
inline constexpr uint32_t recent_rounds_max = 3;
inline constexpr uint32_t retained_rounds_max =
    recent_rounds_max + 1; // Superseded user publication.
inline constexpr uint32_t turn_text_max = 384;
inline constexpr uint32_t summary_text_max = 512;
inline constexpr uint32_t state_bytes_max = 4096;
inline constexpr uint32_t prompt_bytes_max = 8192;

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
    static ayni_status_t begin(const ConversationRecord &record, ayni_string_v1 user,
                               Encoded &parameters, uint64_t memory_id = 0);
    // Extract independent user input from a validated turn, without exposing the wire layout.
    static ayni_status_t input(ayni_bytes_v1 parameters, Text<turn_text_max> &out);
    static ayni_status_t finish(const ConversationRecord &current, ayni_bytes_v1 checkpoint,
                                ConversationRecord &candidate);
    // Preserve a superseded publication under its original turn identity, without its answer.
    static ayni_status_t retain_published_input(const ConversationRecord &, uint64_t,
                                                ConversationRecord &);
    static ayni_status_t erase(const ConversationRecord &current, ConversationRecord &candidate);
    // Single/historical turn deletion clears generated text and preserves other retained users.
    static ayni_status_t erase_round(const ConversationRecord &, uint64_t, ConversationRecord &);
    static ayni_status_t erase_derived(const ConversationRecord &, ConversationRecord &);
    static ayni_status_t encode(const ConversationRecord &record, Encoded &out);
    static ayni_status_t decode(ayni_bytes_v1 bytes, ConversationRecord &out);
    static ayni_status_t validate_turn(void *, ayni_bytes_v1 parameters);
};

// Immutable concrete call; all strings/fields borrow the trusted configuration.
struct Call {
    ayni_policy_intent_kind_t kind = AYNI_POLICY_REQUEST_CALL;
    ayni_string_v1 binding_id{};
    uint32_t binding_version = 1;
    ayni_scope_v1 arguments{sizeof(ayni_scope_v1), 0, nullptr};
    ayni_string_v1 text{};
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
// Each request then states the local date, weekday and time in timezone, so the model resolves
// relative dates itself, marking an untrusted sample as unsynchronized; without any sample or a
// zone it states that the time is unknown.
// utc_now must not block. timezone is a POSIX TZ rule, borrowed like the rest of the config.
struct LocalClock {
    void *context = nullptr;
    void (*utc_now)(void *, ayni_utc_sample_v1 *) = nullptr;
    ayni_string_v1 timezone{};
};
// Both calls are MODEL class; summary binding must have a text result contract.
// model.text is a trusted instruction prefix. Other user text is in parameters.
struct ToolLoopConfig {
    Identity identity;
    Call model;
    Call summary;
    LocalClock clock;
};
class ToolLoop {
  public:
    ayni_status_t configure(const ToolLoopConfig &config);
    ayni_strategy_v1 describe();

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    ToolLoopConfig config_{};
    bool configured_ = false;
    std::array<char, prompt_bytes_max> prompt_{};
};
} // namespace ayni::strategies
#endif
