// Model-facing schedule tool (ADR-0072). Kernel stays the owner of Goals and their authority;
// this Provider only turns one tool operation into at most one owner-approved Goal request.
#ifndef AYNI_APPLICATIONS_ASSISTANT_SCHEDULE_TOOL_H
#define AYNI_APPLICATIONS_ASSISTANT_SCHEDULE_TOOL_H
#include <array>
#include <ayni/applications/assistant.h>
namespace ayni::applications::assistant {
// Bytes. Input covers the longest canonical reminder; results add a Goal ID and its state.
inline constexpr uint32_t schedule_text_max = 640, schedule_result_max = 768;
// Goal admission supplied by the runtime and called only from ScheduleTool::settle, on the
// Kernel owner thread outside step. OK writes the new Goal ID. BUSY asks for a later retry.
// INVALID_STATE means admission has closed for good (shutdown). Any other status is a refusal
// that created no Goal.
struct GoalPort {
    void *context = nullptr;
    ayni_status_t (*create)(void *, const ayni_goal_create_v1 *, uint64_t *goal_id) = nullptr;
};
// Retained Goal registered by this exact tool operation; NOT_FOUND when none is retained.
// Owner thread, outside callbacks.
ayni_status_t find_operation(const ayni_kernel_t *, uint64_t operation_id, uint64_t &goal_id);
// Local TOOL Provider of capability "assistant.schedule" on endpoint "assistant.local". Its
// callbacks run on the Kernel owner thread and only copy a request into a bounded slot, so it
// needs no lock. settle() decides each request once, through GoalPort, and posts the receipt:
//   registered  COMPLETED, "registered goal N: <canonical schedule>; <approval state>"
//   refused     COMPLETED, "rejected: ...", so the model can ask the user and call again
//   no port, or admission closed  FAILED with effect NONE
// An operation that already registered a retained Goal is answered with that Goal instead of
// creating another. There is no remote cancel or query: an attempt in flight at a crash stays
// UNKNOWN, and the owner resolves it with the Goal list.
class ScheduleTool {
  public:
    static constexpr uint32_t request_capacity = 4;
    ScheduleTool();
    ScheduleTool(const ScheduleTool &) = delete;
    ScheduleTool &operator=(const ScheduleTool &) = delete;
    // Borrowed by Core until Kernel destruction; the tool must outlive it.
    ayni_provider_v1 describe();
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
        uint32_t input_size = 0, result_size = 0;
        std::array<char, schedule_text_max> input{};
        std::array<char, schedule_result_max> result{};
        ayni_provider_result_v1 receipt{}; // Valid once answered; result views the array above.
    };
    std::array<Slot, request_capacity> slots_{};
    ayni_call_capability_v1 capability_{};
    ayni_status_t answer(const ayni_kernel_t *, GoalPort, Slot &);
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
};
} // namespace ayni::applications::assistant
#endif
