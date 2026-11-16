// Local policy helpers and the versioned turn state, shared by both policies.
#ifndef AYNI_STRATEGIES_SUPPORT_H
#define AYNI_STRATEGIES_SUPPORT_H
#include <ayni/detail/workspace.hpp>
#include <ayni/strategies/builtin.h>
#include <ayni/values_json.hpp>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ayni::strategies::detail {
using Workspace = ::ayni::detail::Workspace;
// Begins one owned JSON document; Writer borrows this string only through synchronous writes.
json::Writer growing_json(std::string &output);
inline constexpr uint32_t max_model_rounds = AYNI_MAX_TURNS_MAX;

// One immutable admitted model group. Text borrows encoded; actual node prefixes
// have checked allocations and fixed proposal slots. No result or authority is duplicated here.
struct ModelProposals {
    struct Proposal {
        ayni_tool_call_v1
            call{}; // Text borrows encoded; node_count names owned nodes, pointer null.
        std::unique_ptr<ayni_argument_node_v1[]> nodes; // Actual prefix only, <=VALUE_NODE_MAX.
        ayni_tool_call_v1 view() const {
            auto value = call;
            if (value.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
                value.input.arguments.nodes = nodes.get();
            }
            return value;
        }
    };
    uint64_t call = 0, sequence = 0; // Exact terminal source identity and immutable fact sequence.
    BodyRef body{};      // Complete source reference; absent for inline Core-owned results.
    std::string encoded; // Owns all call/assistant text until group retirement.
    std::array<Proposal, AYNI_CALL_TOOL_COUNT_MAX> calls;
    uint32_t count = 0;         // Admitted prefix; 1..CALL_TOOL_COUNT_MAX after factory success.
    ayni_string_v1 assistant{}; // Borrows encoded after complete list admission.
};
// A checked, synchronous shared owner for immutable derived representations.
// Copies share data across a Turn's transactional working copy; no control-block allocation.
template <class T> class Prepared {
    struct State {
        T value;
        uint32_t owners = 1; // Bounded owner-thread handles; never shared across threads.
    };

  public:
    Prepared() = default;
    static Prepared create() { return Prepared(new (std::nothrow) State()); }
    Prepared(const Prepared &other) : value_(other.value_) {
        if (value_) {
            ++value_->owners;
        }
    }
    Prepared &operator=(const Prepared &other) {
        if (this != &other) {
            Prepared copy(other);
            std::swap(value_, copy.value_);
        }
        return *this;
    }
    ~Prepared() { reset(); }
    void reset() {
        if (value_ && --value_->owners == 0U) {
            delete value_;
        }
        value_ = nullptr;
    }
    // Factory-only mutable access, refused once published to another handle.
    T *building() { return value_ && value_->owners == 1U ? &value_->value : nullptr; }
    const T *get() const { return value_ ? &value_->value : nullptr; }
    const T *operator->() const { return get(); }
    const T &operator*() const { return *get(); }
    explicit operator bool() const { return value_ != nullptr; }

  private:
    explicit Prepared(State *value) : value_(value) {}
    State *value_ = nullptr; // Null owns nothing; one allocation includes T and its owner count.
};
using PreparedProposals = Prepared<ModelProposals>;
// Only the active large-result summary owns a projection. Normal replies retain references,
// not this text. Retirement/expiry drops it with the existing bounded Turn slot.
struct ResultProjection {
    BodyRef source{}; // Exact view source including digest/size; absent before factory success.
    uint64_t call = 0, sequence = 0; // Core fact identity checked before every reuse.
    std::string text;                // One complete rendering, shared across chunk decisions.
};
enum class TurnPhase : uint32_t { initial, ready, model, tools, summary, result_summary };
struct SummaryProgress {
    uint32_t offset = 0; // Bytes already covered by the last successful chunk.
    uint32_t next = 0;   // End of the outstanding chunk; offset<=next<=original length.
    uint32_t chunk = 0;  // Monotone summary Call ordinal within its source.
    std::string text;    // Unconfirmed candidate; originals remain in the body store.
};
// Immutable retained round metadata. The conversation store owns the referenced bodies;
// the execution path loads text only when reconstructing missing history or displaying it.
struct RoundReferences {
    uint64_t number = 0;      // Monotone conversation ordinal; nonzero in an admitted index.
    BodyRef user{}, answer{}; // User is present; answer is absent for retained input only.
};
// The checkpoint carries the immutable base revision and candidate context; done means
// a final answer was appended, never accepted as input parameters.
// A decoded turn. Mutable request/history text lives here; retained rounds stay references.
// Encoding persists changed text and retains original immutable round identities.
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
    std::vector<RoundReferences> rounds;  // record.rounds, oldest first.
    TurnPhase phase = TurnPhase::initial; // Sole owner of current model/group/compaction progress.
    uint32_t model_round = 0;    // Monotone main Call ordinal, never reset on context invalidation.
    uint32_t tool_index = 0;     // Next proposal in the current durable model result.
    uint32_t group_start = 0;    // History byte boundary before pending assistant/tool group.
    uint32_t compact_prefix = 0; // Completed prefix selected for compaction; zero when inactive.
    SummaryProgress history_summary;
    Prepared<std::string> history_projection; // One canonical prefix per compaction, never saved.
    uint32_t compact_generation = 0; // Monotone summary operation generation within this Task.
    TurnPhase compact_resume = TurnPhase::ready; // Controller to resume after atomic replacement.
    SummaryProgress result_summary;
    struct ResultSource {
        BodyRef view{}; // Shared original result/proof graph, present only in result_summary.
        uint32_t json_bytes = 0; // Complete projection size; offset/next never exceed it.
    } result_source;
    Prepared<ResultProjection>
        result_projection;       // Transient, never serialized or independently saved.
    PreparedProposals proposals; // Current source group; no authorization cache.
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
// Conversation-owned content requests background persistence when using stage as write.
// Model request temporaries deliberately keep put_body's unconfirmed lifetime.
ayni_status_t save_body(const ayni_body_store_v1 &, uint64_t scope, ayni_string_v1, BodyRef &);
ayni_status_t get_body(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out);
// One synchronous immutable read operation. Reuses verified small bodies within a
// fixed scratch budget; large payload handles expire after their current consumer.
// Nothing survives an owner call or a Core/privacy mutation.
class BodyReader {
  public:
    // Borrow a stable cached string, or own one streamed payload. This handle stays
    // inside the reader's synchronous lifetime; no shared control block is needed.
    struct Body {
        Body() = default;
        Body(const Body &) = delete;
        Body &operator=(const Body &) = delete;
        std::string owned;
        const std::string *value = nullptr;
        const std::string &operator*() const { return *value; }
        const std::string *operator->() const { return value; }
        const std::string *get() const { return value; }
        explicit operator bool() const { return value != nullptr; }
    };
    // Keep additional ESP scratch small. Exhaustion uses uncached reads, never rejects data.
    static constexpr size_t cache_bytes = 16U * 1024U, cache_entries = 64;
    explicit BodyReader(const ayni_body_store_v1 &store) : store_(store) {}
    BodyReader(const BodyReader &) = delete;
    BodyReader &operator=(const BodyReader &) = delete;
    // A successful handle survives map inserts and expires before this reader is destroyed.
    ayni_status_t read(const BodyRef &, Body &);
    // Copies a final field once; large uncached fields move directly from the store read.
    ayni_status_t copy(const BodyRef &, std::string &);
    // Text fields additionally require valid UTF-8; small cached bodies are checked once.
    ayni_status_t copy_text(const BodyRef &, std::string &);
    // Requests all independent references; uncached payloads are released each iteration.
    ayni_status_t prepare(const std::vector<BodyRef> &);
    // Owned cache capacity bytes, excluding bounded map/handle overhead; zero when empty.
    size_t retained_bytes() const { return retained_; }

  private:
    struct Cached {
        BodyRef reference; // Complete immutable identity, digest and byte length admitted at read.
        std::string bytes; // Verified immutable bytes, borrowed only within this reader.
        bool text_checked = false; // True only after these bytes passed UTF-8 validation.
    };
    const ayni_body_store_v1 &store_;   // Borrowed only through this synchronous operation.
    std::map<uint64_t, Cached> loaded_; // Successful small reads, bounded by both limits above.
    size_t retained_ = 0; // Sum of cached string capacities; never exceeds cache_bytes.
};
// One writer/reader for the retained reference index; no body staging or text projection.
void encode_rounds(const std::vector<RoundReferences> &, std::string &out);
ayni_status_t round_references(BodyReader &, const ConversationRecord &,
                               std::vector<RoundReferences> &out);
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
// Resolved views live through this synchronous decision; proposal ownership can be reused
// across decisions, but the current Core result and privacy are still queried each time.
struct ModelResult {
    ModelResult() = default;
    ModelResult(const ModelResult &) = delete;
    ModelResult &operator=(const ModelResult &) = delete;
    ayni_task_call_result_v1 call{};
    std::string owned;
    PreparedProposals proposals;
    model::Continuation continuation{};
};
// result's status; a completed body still loading answers NOT_READY.
ayni_status_t model_result(Workspace &workspace, const ayni_body_store_v1 &bodies,
                           const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                           ModelResult &out,
                           model::ReplyEncoding encoding = model::ReplyEncoding::text,
                           PreparedProposals *prepared = nullptr);
// Resolve the original normalized TEXT/VALUE body for tools, managed reads and archives.
// owned keeps every returned view alive; model proposal/continuation uses model_result instead.
ayni_status_t resolve_result_body(const ayni_body_store_v1 &, ayni_task_call_result_v1 &,
                                  std::string &owned);
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
