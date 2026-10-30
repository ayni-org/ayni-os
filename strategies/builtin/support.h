// Local policy helpers and the versioned turn state, shared by both policies.
#ifndef AYNI_STRATEGIES_SUPPORT_H
#define AYNI_STRATEGIES_SUPPORT_H
#include <ayni/strategies/builtin.h>
#include <initializer_list>
#include <string_view>

namespace ayni::strategies::detail {
inline constexpr uint32_t max_model_rounds = AYNI_TASK_MODEL_CALL_HARD_LIMIT_MAX;

// The checkpoint carries the immutable base revision and a candidate context.
// done means the final answer was appended; never accepted as input parameters.
enum class TurnPhase : uint32_t { initial, ready, model, tools, summary, result_summary };
struct SummaryProgress {
    uint32_t offset = 0;         // Bytes already covered by the last successful chunk.
    uint32_t next = 0;           // End of the outstanding chunk; offset<=next<=original length.
    uint32_t chunk = 0;          // Monotone summary Call ordinal within its source.
    Text<summary_text_max> text; // Unconfirmed candidate; originals remain in SQLite/history.
};
struct Turn {
    ConversationRecord record;
    uint64_t base_revision = 0;
    uint64_t memory_id = 0; // Optional explicit source selected for this turn; zero selects none.
    Text<turn_text_max> input;
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
};
ayni_string_v1 model_key(uint32_t round, char *buffer);
ayni_string_v1 function_key(uint64_t source, uint32_t index, char *buffer);
ayni_status_t read_turn(ayni_bytes_v1 bytes, Turn &out);
ayni_status_t write_turn(const Turn &turn, uint8_t *data, uint32_t capacity, uint32_t &size);
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
ayni_status_t request(const Call &call, ayni_string_v1 key, ayni_string_v1 text,
                      ayni_policy_output_v1 &output);
ayni_status_t result(const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                     ayni_task_call_result_v1 &out);
// Known failure stops the sequence; unknown is a hold, never completion/failure.
ayni_status_t outcome(const ayni_task_call_result_v1 &result);

// Checked text assembly. Overflow is sticky and never truncates silently.
class Writer {
  public:
    Writer(char *data, uint32_t capacity) : data_(data), capacity_(capacity) {}
    // Opens a model body: the parts form the instructions, kept apart from the data added next
    // (AYNI_MODEL_INSTRUCTIONS_PREFIX, ADR-0074).
    void instructions(std::initializer_list<ayni_string_v1> parts);
    void add(ayni_string_v1 text);
    void add(std::string_view text) { add(view(text)); }
    void number(uint64_t value);
    void integer(int64_t value);
    bool ok() const { return ok_; }
    ayni_string_v1 text() const { return {data_, size_}; }

  private:
    char *data_;
    uint32_t capacity_;
    uint32_t size_ = 0;
    bool ok_ = true;
};
} // namespace ayni::strategies::detail
#endif
