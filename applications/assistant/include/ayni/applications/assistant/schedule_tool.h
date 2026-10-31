// Model-facing schedule tool (ADR-0072). Kernel stays the owner of Goals and their authority;
// this Provider only turns one tool operation into at most one owner-approved Goal request.
#ifndef AYNI_APPLICATIONS_ASSISTANT_SCHEDULE_TOOL_H
#define AYNI_APPLICATIONS_ASSISTANT_SCHEDULE_TOOL_H
#include <array>
#include <ayni/applications/assistant.h>
#include <ayni/calendar.h>
#include <ayni/tool_usage.h>
#include <ayni/values.hpp>
#include <vector>
namespace ayni::applications::assistant {
// Longest canonical schedule text, "remind weekly YYYY-MM-DDTHH:MM <zone> <text>", in bytes.
inline constexpr uint32_t schedule_canonical_max =
    sizeof("remind weekly YYYY-MM-DDTHH:MM  ") - 1 + AYNI_CALENDAR_TZ_MAX_BYTES + text_bytes_max;
// Portable structured input/result byte budgets, including value framing.
inline constexpr uint32_t schedule_text_max = 1024, schedule_result_max = 1024;
inline constexpr uint32_t schedule_input_nodes = 7, schedule_result_nodes = 6;
inline constexpr uint32_t schedule_default_fields = schedule_input_nodes - 2;
// Model Goals that may wait for owner approval at once. Chat turns and owner Goals share the
// Goal table, so a misbehaving model must not be able to fill it.
inline constexpr uint32_t model_goals_waiting_max = 4;
// Goal admission supplied by the runtime and called only from ScheduleTool::settle, on the
// Kernel owner thread outside step. OK writes the new Goal ID. BUSY asks for a later retry.
// INVALID_STATE means admission has closed for good (shutdown). Any other status is a refusal
// that created no Goal.
struct GoalPort {
    void *context = nullptr;
    ayni_status_t (*create)(void *, const ayni_goal_create_v1 *, uint64_t *goal_id) = nullptr;
};
// Retained schedule Goals of model origin: the one registered by operation_id (0 when none is
// retained; pass operation 0 to match none) and every one still waiting for owner approval, in
// ID order. Owner thread, outside callbacks. BUSY while a tracked creation is not yet durable.
struct ModelGoals {
    uint64_t registered = 0;
    std::vector<uint64_t> waiting;
};
ayni_status_t model_goals(const ayni_kernel_t *, uint64_t operation_id, ModelGoals &);
// Local TOOL Provider of capability "assistant.schedule" on endpoint "assistant.local". Its
// callbacks run on the Kernel owner thread and only copy a request into a bounded slot, so it
// needs no lock. settle() decides each request once, through GoalPort, and posts the receipt:
//   registered/refused COMPLETED with structured status, nullable Goal ID, state,
//                canonical schedule and explanation; COMPLETE means the tool answered.
//                Refusal lets the model correct its request and continue the turn.
//   turn ended   CANCELLED, effect NONE: the calling Task stopped before the decision
//   no port, or admission closed   FAILED, effect NONE
// An operation decided again after its Goal was registered (an error interrupted the decision)
// answers with that Goal; Core never resubmits a NOT_IDEMPOTENT call by itself. Each new model
// tool call is a new operation, so model_goals_waiting_max bounds what the model can register.
// There is no remote cancel or query: an attempt in flight at a crash stays UNKNOWN, and the
// owner resolves it with the Goal list.
class ScheduleTool {
  public:
    static constexpr uint32_t request_capacity = 4;
    ScheduleTool();
    ScheduleTool(const ScheduleTool &) = delete;
    ScheduleTool &operator=(const ScheduleTool &) = delete;
    // Since 0.53: borrow a queried value workspace before describing/registering.
    ayni_status_t configure_workspace(const ayni_workspace_v1 *workspace);
    // Borrowed by Core until Kernel destruction; the tool must outlive it.
    ayni_provider_v1 describe();
    // Runtime supplies its exact Binding identity; strings borrow that identity
    // and immutable extension text. No registration, I/O or Core lookup.
    ayni_tool_usage_v1 usage(ayni_tool_ref_v1 binding) const;
    // Owner thread, outside step. Requests the port defers stay queued; a receipt Core cannot
    // take yet (BUSY) is posted again later and never re-executed. Other post failures return.
    ayni_status_t settle(const ayni_kernel_t *, GoalPort);
    uint32_t pending() const; // Requests whose receipt Core has not accepted yet.

  private:
    // empty -> queued (copied request) -> answered (receipt fixed) -> empty (posted).
    enum class Phase { empty, queued, answered };
    struct Slot {
        Phase phase = Phase::empty;
        ayni_attempt_ref_v1 attempt{};
        uint64_t call = 0, operation = 0;
        ayni_provider_completion_t *completion = nullptr;
        uint32_t input_size = 0;
        std::array<uint8_t, schedule_text_max> input{};
        std::array<char, schedule_canonical_max + 1> canonical{};
        ayni::ValueBuilder<schedule_result_nodes> result{};
        ayni_provider_result_v1
            receipt{}; // Valid once answered; value borrows builder/canonical above.
    };
    ayni_workspace_v1 workspace_{}; // Exclusive value-codec bytes; null until configuration.
    std::array<Slot, request_capacity> slots_{};
    ayni_call_capability_v1 capability_{};
    std::array<ayni_schema_node_v1, schedule_input_nodes> input_schema_{};
    std::array<ayni_schema_node_v1, schedule_result_nodes> result_schema_{};
    std::array<ayni_value_node_v1, schedule_default_fields> defaults_{};
    std::array<ayni_value_v1, schedule_default_fields> default_views_{};
    ayni_business_contract_v1 business_{};
    void build_input_schema();  // Owns input Schema/default storage.
    void build_result_schema(); // Owns the structured receipt declaration.
    ayni_status_t complete(const ayni_kernel_t *, Slot &, uint64_t, const std::string &,
                           const char *); // Publishes slot-owned structured receipt.
    ayni_status_t answer(const ayni_kernel_t *, GoalPort, Slot &);
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
};
} // namespace ayni::applications::assistant
#endif
