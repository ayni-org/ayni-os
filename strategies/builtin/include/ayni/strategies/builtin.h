// Policies and conversation values (ADR-0056). No I/O beyond an injected nonblocking clock
// (ADR-0073) and the host body store (ADR-0096), no Kernel internals. Message texts live in the
// body store; records, Goal parameters and checkpoints hold only references and control state.
// Decoded turns have a bounded cache; the owner maintains it before history/privacy ACKs.
// Other working copies are allocated on demand and freed before a call returns (ADR-0095).
// Objects and borrowed configuration live through Kernel destroy; one owner thread uses each
// strategy.
#ifndef AYNI_STRATEGIES_BUILTIN_H
#define AYNI_STRATEGIES_BUILTIN_H
#include <array>
#include <ayni/bodies.h>
#include <ayni/calendar.h>
#include <ayni/calls.h>
#include <ayni/directory.h>
#include <ayni/memory.h>
#include <ayni/model_messages.hpp>
#include <ayni/workspace.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ayni::strategies {
inline constexpr uint32_t rule_count_max = 8;
inline constexpr uint32_t rule_steps_max = 16;

using BodyRef = ayni_body_ref_v1;
// Fixed encodings: a conversation record or one turn's parameters/checkpoint. Texts are body
// references, so the size no longer depends on message length.
inline constexpr uint32_t encoded_bytes_max = 2048;
struct Encoded {
    std::array<uint8_t, encoded_bytes_max> bytes{};
    uint32_t size = 0;
    ayni_bytes_v1 view() const { return {bytes.data(), size}; }
};
// One retained user turn and its final answer; tools remain in the Task call ledger.
struct Round {
    uint64_t number = 0;
    std::string user;
    std::string answer;
};
// Caller-owned conversation, isolated by id/owner/generation. Summary is model
// inference for this conversation only, with inclusive source turn numbers. Every text is a
// body in the store, in scope id; an empty reference means no text.
struct ConversationRecord {
    uint64_t id = 0;
    uint64_t owner_instance_id = 0;
    uint64_t generation = 1;
    uint64_t revision = 0;
    uint64_t last_round = 0;
    uint64_t summary_first = 0;
    uint64_t summary_last = 0;
    BodyRef summary{};
    uint32_t history_bytes = 0; // Composed framing bytes; zero for explicit raw histories.
    BodyRef history{};          // Complete standard messages, without fixed system instructions.
    ayni_model_usage_v1 last_usage{}; // Last serving-model observation; NONE after context reset.
    uint64_t last_request_characters = 0; // Unicode characters in the last sent messages/tools.
    uint64_t last_input_estimate =
        0; // Estimated input tokens for that request; never service usage.
    ayni_capability_context_v1
        disclosure{};             // Conservative retained metadata visibility, zero before use.
    uint64_t context_version = 1; // Positive generation; changes when derived context is rebuilt.
    // Independent accepted input survives invalidation before publication.
    BodyRef draft{};
    BodyRef rounds{}; // Retained rounds, oldest first; read with Conversation::rounds.
    uint32_t round_count = 0;
};

// An outer store persists the candidate *before* accepting it. begin/finish are
// non-mutating with respect to that record: failure leaves it intact. erase increments
// generation so a late completed Task cannot resurrect deleted context. Shared Session
// and Kernel context intents own the cross-store deletion barrier (ADR-0060/0108).
class Conversation {
  public:
    // Since 0.53: every synchronous codec call borrows a queried exclusive region.
    static ayni_status_t workspace_requirements(size_t *bytes, size_t *alignment);
    // Writes draft+user as one input body; NOT_READY while the store is not yet durable.
    static ayni_status_t begin(const ayni_workspace_v1 *workspace, const ayni_body_store_v1 &bodies,
                               const ConversationRecord &record, ayni_string_v1 user,
                               Encoded &parameters, uint64_t memory_id = 0);
    // Reference to independent user input of a validated turn, without exposing the wire layout.
    static ayni_status_t input(const ayni_workspace_v1 *workspace, ayni_bytes_v1 parameters,
                               BodyRef &out);
    static ayni_status_t finish(const ayni_workspace_v1 *workspace,
                                const ConversationRecord &current, ayni_bytes_v1 checkpoint,
                                ConversationRecord &candidate);
    // Preserve a superseded publication under its original turn identity, without its answer.
    static ayni_status_t retain_published_input(const ayni_workspace_v1 *workspace,
                                                const ayni_body_store_v1 &bodies,
                                                const ConversationRecord &, uint64_t,
                                                ConversationRecord &);
    static ayni_status_t erase(const ayni_workspace_v1 *workspace,
                               const ConversationRecord &current, ConversationRecord &candidate);
    // Single/historical turn deletion clears generated text and preserves other retained users.
    static ayni_status_t erase_round(const ayni_workspace_v1 *workspace,
                                     const ayni_body_store_v1 &bodies, const ConversationRecord &,
                                     uint64_t, ConversationRecord &);
    static ayni_status_t erase_derived(const ayni_workspace_v1 *workspace,
                                       const ayni_body_store_v1 &bodies, const ConversationRecord &,
                                       ConversationRecord &);
    static ayni_status_t encode(const ayni_workspace_v1 *workspace,
                                const ConversationRecord &record, Encoded &out);
    static ayni_status_t decode(const ayni_workspace_v1 *workspace, ayni_bytes_v1 bytes,
                                ConversationRecord &out);
    // Matches a retained Goal's original input to this exact conversation baseline.
    // NOT_FOUND means another baseline; success only identifies work, never re-executes it.
    static ayni_status_t pending_input(const ayni_workspace_v1 *, const ConversationRecord &,
                                       ayni_bytes_v1 parameters, BodyRef &);
    // GoalType validator context is a borrowed ayni_workspace_v1 descriptor.
    // Use a wrapper when other GoalType callbacks require a different context.
    static ayni_status_t validate_turn(void *, ayni_bytes_v1 parameters);
    // Body reads verify length and digest; NOT_READY while the store is still loading.
    static ayni_status_t text(const ayni_body_store_v1 &bodies, const BodyRef &ref,
                              std::string &out);
    // Resolves this record's ordered history index; ordinary text() never interprets body contents.
    static ayni_status_t history(const ayni_body_store_v1 &, const ConversationRecord &,
                                 std::string &);
    static ayni_status_t rounds(const ayni_body_store_v1 &bodies, const ConversationRecord &record,
                                std::vector<Round> &out);
    // Reads only the latest answer payload after checking the round index. NOT_FOUND
    // means no retained round; NOT_READY preserves out while its body is loading.
    static ayni_status_t latest_answer(const ayni_body_store_v1 &, const ConversationRecord &,
                                       std::string &out);
    // Direct record roots only. Collection must use the transitive body-store overload below.
    static void references(const ConversationRecord &record, std::vector<BodyRef> &out);
    // Includes the transitive shared payloads in the message index; NOT_READY defers collection.
    static ayni_status_t references(const ayni_body_store_v1 &, const ConversationRecord &,
                                    std::vector<BodyRef> &);
    // Bodies a turn's parameters or checkpoint name, in addition to its record's.
    static ayni_status_t turn_references(const ayni_workspace_v1 *workspace, ayni_bytes_v1 bytes,
                                         std::vector<BodyRef> &out);
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
// Body-store progress only. All tool results use the same shared reference representation.
struct BodyProgress {
    void *context = nullptr;                            // Borrowed through strategy destruction.
    uint32_t (*ready)(void *, uint64_t task) = nullptr; // Null: no asynchronous body work.
};
// The model binding's token settings (ADR-0095), all positive. A request holds at most
// context_window_tokens; max_output_tokens of it are left for the answer. Compaction starts when
// the estimated input would leave no room for one tool result of tool_result_max_tokens and
// shrinks the context to compact_target_tokens, which also bounds the retained user inputs.
struct TokenLimits {
    uint64_t context_window_tokens = 0;
    uint64_t max_output_tokens = 0;
    uint64_t compact_target_tokens = 0;
    uint64_t tool_result_max_tokens = 0;
};
// Discovery preserves existing consumers; preloaded sends the current authorized directory
// as request-only tools. Neither mode changes Core authorization.
enum class ToolExposure { discovery, preloaded };
enum class Persistence { durable, background };
struct ToolLoopConfig {
    Identity identity;
    Call model;
    Call summary;
    LocalClock clock;
    Call shell; // Optional trusted exact Shell selection; empty Binding means unavailable.
    TokenLimits limits;
    // Required store for every conversation text and model request body, borrowed like the
    // rest of the config. Bodies of conversation id use scope id.
    const ayni_body_store_v1 *bodies = nullptr;
    BodyProgress body_progress{};
    Persistence persistence = Persistence::durable; // Background is explicit and requires staging.
    // Active turns and at most one completed graph per conversation share this bound.
    uint32_t cached_turns = 4;                            // Eviction never drops store ownership.
    ToolExposure tool_exposure = ToolExposure::discovery; // Captured anew on every decision.
    model::ReplyEncoding reply_encoding = model::ReplyEncoding::text; // Main Provider encoding.
    model::ReplyEncoding summary_reply_encoding = model::ReplyEncoding::text; // Summary encoding.
    model::Projection projection{}; // Actual wire context, including tools and continuation.
};
class ToolLoop {
  public:
    ToolLoop();
    ~ToolLoop();
    ToolLoop(const ToolLoop &) = delete;
    ToolLoop &operator=(const ToolLoop &) = delete;
    // Declared checkpoint and turn-parameter size; independent of message length.
    static constexpr uint32_t checkpoint_bytes = encoded_bytes_max;
    // Since 0.53: the region remains alive through Kernel destruction. Its scratch
    // borrow lasts only for configure/decide. One logical owner may reuse the same
    // region for other strategies/codecs whose synchronous calls cannot overlap;
    // callbacks running inside a decide need separate scratch (workspace.h).
    static ayni_status_t workspace_requirements(size_t *bytes, size_t *alignment);
    ayni_status_t configure(const ToolLoopConfig &config, const ayni_workspace_v1 *workspace);
    ayni_strategy_v1 describe();
    // Owner thread, outside Core callbacks, after stepping and before history/privacy ACKs.
    // Uses public Task metadata; privacy progress clears even dormant WAITING copies.
    // Cancelled/failed copies retire; completed graphs may serve the next exact-source turn.
    // A completed privacy generation is consumed once, so later valid turns keep their cache.
    ayni_status_t maintain(const ayni_kernel_t *kernel, const ayni_privacy_info_v1 &privacy);
    size_t
    cached_turns() const; // Count of decoded private turns, excluding immutable tool schemas.

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static uint32_t ready(void *, uint64_t);
    struct Cache; // Owns decoded turns and immutable tool JSON; never authority decisions.
    std::unique_ptr<Cache> cache_;
    ToolLoopConfig config_{};
    bool configured_ = false;
    ayni_workspace_v1 workspace_{}; // Borrowed bytes; present only after successful configure.
};
} // namespace ayni::strategies
#endif
