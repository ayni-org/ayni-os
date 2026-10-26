// Local policy helpers and the versioned turn state, shared by both policies.
#ifndef AYNI_STRATEGIES_SUPPORT_H
#define AYNI_STRATEGIES_SUPPORT_H
#include <ayni/strategies/builtin.h>
#include <string_view>

namespace ayni::strategies::detail {
inline constexpr uint32_t checkpoint_format = 1;
inline constexpr uint32_t summary_drop_count = 2;
inline constexpr uint32_t max_model_rounds = AYNI_TASK_MODEL_CALL_HARD_LIMIT_MAX;
inline constexpr std::string_view summary_step = "dialog.summary.v1";

// The checkpoint carries the immutable base revision and a candidate context.
// done means the final answer was appended; never accepted as input parameters.
struct Turn {
    ConversationRecord record;
    uint64_t base_revision = 0;
    Text<turn_text_max> input;
    bool done = false;
};
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
