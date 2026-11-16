// Local policy helpers and the versioned turn state, shared by both policies.
#ifndef AYNI_STRATEGIES_SUPPORT_H
#define AYNI_STRATEGIES_SUPPORT_H
#include <ayni/detail/workspace.hpp>
#include <ayni/strategies/builtin.h>
#include <ayni/values_json.hpp>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace ayni::strategies::detail {
using Workspace = ::ayni::detail::Workspace;
// Begins one owned JSON document; Writer borrows this string only through synchronous writes.
json::Writer growing_json(std::string &output);
inline constexpr uint32_t max_model_rounds = AYNI_MAX_TURNS_MAX;

// The checkpoint carries the immutable base revision and a candidate context.
// done means the final answer was appended; never accepted as input parameters.
enum class TurnPhase : uint32_t { initial, ready, model, tools, summary, result_summary };
struct SummaryProgress {
    uint32_t offset = 0; // Bytes already covered by the last successful chunk.
    uint32_t next = 0;   // End of the outstanding chunk; offset<=next<=original length.
    uint32_t chunk = 0;  // Monotone summary Call ordinal within its source.
    std::string text;    // Unconfirmed candidate; originals remain in the body store.
};
// A decoded turn. Its texts are working copies of bodies; the encoding holds only their
// references (refs), refreshed by persist before every encode.
struct Turn {
    ConversationRecord record;
    uint64_t base_revision = 0;
    uint64_t memory_id = 0; // Optional explicit source selected for this turn; zero selects none.
    std::string input;
    std::string definitions;    // Transient request-only tools; regenerated from each snapshot.
    uint32_t history_saved = 0; // Appended prefix already represented by history_index.
    std::map<std::string, BodyRef>
        result_views;          // New tool protocol IDs -> derived views of shared Call results.
    std::string history_index; // Owned small ordered framing/reference index, never full text.
    std::string history;       // record.history's messages, standard framing.
    std::string summary;       // record.summary.
    std::vector<Round> rounds; // record.rounds, oldest first.
    TurnPhase phase = TurnPhase::initial; // Sole owner of current model/group/compaction progress.
    uint32_t model_round = 0;    // Monotone main Call ordinal, never reset on context invalidation.
    uint32_t tool_index = 0;     // Next proposal in the current durable model result.
    uint32_t group_start = 0;    // History byte boundary before pending assistant/tool group.
    uint32_t compact_prefix = 0; // Completed prefix selected for compaction; zero when inactive.
    SummaryProgress history_summary;
    uint32_t compact_generation = 0; // Monotone summary operation generation within this Task.
    TurnPhase compact_resume = TurnPhase::ready; // Controller to resume after atomic replacement.
    SummaryProgress result_summary;
    ArchivedResult archived_result; // Active only in result_summary; exact immutable original.
    bool done = false;
    struct Refs {
        BodyRef input{}, history_summary{}, result_summary{};
    } refs; // Body references of input and the two summary candidates.
};
// Reads every body the encoded turn names. NOT_READY when the store is still loading.
ayni_status_t read_turn(Workspace &workspace, const ayni_body_store_v1 &bodies, ayni_bytes_v1 bytes,
                        Turn &out, const std::vector<const Turn *> &completed = {});
// Writes each changed text as a body, then encodes references and control state. NOT_READY
// while a body is not yet durable; nothing is encoded then.
ayni_status_t write_turn(Turn &turn, const ayni_body_store_v1 &bodies, uint8_t *data,
                         uint32_t capacity, uint32_t &size);
// Reserved bits clear; the absent reference names no length or digest.
bool valid_ref(const BodyRef &ref);
// Stores bytes in scope and verifies the reference the store returned.
ayni_status_t put_body(const ayni_body_store_v1 &bodies, uint64_t scope, ayni_string_v1 text,
                       BodyRef &out);
ayni_status_t get_body(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out);
// Retained rounds body codec.
ayni_status_t encode_rounds(const ayni_body_store_v1 &, uint64_t scope,
                            const std::vector<Round> &rounds, std::string &out);
ayni_status_t decode_rounds(const ayni_body_store_v1 &, ayni_string_v1 bytes, uint32_t count,
                            uint64_t last_round, std::vector<Round> &out,
                            std::vector<BodyRef> *references = nullptr);
ayni_string_v1 model_key(uint32_t round, char *buffer);
ayni_string_v1 function_key(uint64_t source, uint32_t index, char *buffer);
bool valid_text(ayni_string_v1 value);
bool valid_id(ayni_string_v1 value);
bool equal(ayni_string_v1 a, ayni_string_v1 b);
bool valid_call(const Call &call);
bool valid_identity(const Identity &identity);
ayni_string_v1 view(std::string_view text);
ayni_strategy_v1 descriptor(Identity identity, void *context, ayni_decide_fn decide,
                            uint32_t checkpoint_bytes);
ayni_status_t report(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output,
                     ayni_policy_intent_kind_t kind);
// A MODEL request with a body store sends text by reference, written in scope first (ADR-0096);
// NOT_READY defers the decision until the store has made it durable.
ayni_status_t request(const Call &call, ayni_string_v1 key, ayni_string_v1 text,
                      ayni_policy_output_v1 &output, const ayni_body_store_v1 *bodies = nullptr,
                      uint64_t scope = 0);
ayni_status_t result(const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                     ayni_task_call_result_v1 &out);
// A model result with its body resolved, inline or read and verified from the store. call.text
// is then the answer, or the assistant text beside the tool calls whose list encoding is list;
// call.body is cleared. Views borrow the snapshot or owned, so the object stays in place.
struct ModelResult {
    ModelResult() = default;
    ModelResult(const ModelResult &) = delete;
    ModelResult &operator=(const ModelResult &) = delete;
    ayni_task_call_result_v1 call{};
    ayni_string_v1 list{};
    std::string owned;
    model::Continuation continuation{}; // Borrowed from the selected Provider reply encoding.
};
// result's status; a completed body still loading answers NOT_READY.
ayni_status_t model_result(Workspace &workspace, const ayni_body_store_v1 &bodies,
                           const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                           ModelResult &out,
                           model::ReplyEncoding encoding = model::ReplyEncoding::text);
// Resolve the original normalized TEXT/VALUE body for tools, managed reads and archives.
// owned keeps every returned view alive; model proposal/continuation uses model_result instead.
ayni_status_t resolve_result_body(const ayni_body_store_v1 &, ayni_task_call_result_v1 &,
                                  std::string &owned);
// Proposal index of a resolved list; its strings borrow the list (ayni_tool_call_read).
ayni_status_t read_proposal(Workspace &workspace, ayni_string_v1 list, uint32_t index,
                            ayni_task_tool_call_v1 &out);
// Known failure stops the sequence; unknown is a hold, never completion/failure.
ayni_status_t outcome(const ayni_task_call_result_v1 &result);

// Checked text assembly. Overflow is sticky and never truncates silently. A Writer over a
// string grows it on demand and never overflows.
class Writer {
  public:
    Writer(char *data, uint32_t capacity) : data_(data), capacity_(capacity) {}
    explicit Writer(std::string &out) : grow_(&out) { out.clear(); }
    // Opens a model body: the parts form the instructions, kept apart from the data added next
    // (AYNI_MODEL_INSTRUCTIONS_PREFIX, ADR-0074).
    void instructions(std::initializer_list<ayni_string_v1> parts);
    void add(ayni_string_v1 text);
    void add(std::string_view text) { add(view(text)); }
    void number(uint64_t value);
    void integer(int64_t value);
    bool ok() const { return ok_; }
    ayni_string_v1 text() const { return grow_ ? view(*grow_) : ayni_string_v1{data_, size_}; }

  private:
    char *data_ = nullptr;
    uint32_t capacity_ = 0;
    std::string *grow_ = nullptr;
    uint32_t size_ = 0;
    bool ok_ = true;
};
} // namespace ayni::strategies::detail
#endif
