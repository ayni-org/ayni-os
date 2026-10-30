// Native function routing owns local directory/result rendering, never Kernel state.
#ifndef AYNI_STRATEGIES_HARNESS_H
#define AYNI_STRATEGIES_HARNESS_H
#include "support.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
namespace ayni::strategies::detail {
struct ToolBudget {
    uint64_t tokens = UINT64_MAX; // Estimated remaining input tokens; MAX disables token limiting.
    ayni_string_v1 protocol_id{}; // Current opaque ID, counted with the matching reply envelope.
    bool fits(ayni_string_v1 reply) const {
        return model::estimate_tool_reply(protocol_id, reply) <= tokens;
    }
};
// Select a nonempty UTF-8 prefix under the caller's framing/token predicate.
// The same bounded search serves history and archived-result summary chunks.
template <class Make>
ayni_status_t fit_summary_prefix(ayni_string_v1 source, Make make, uint32_t &chosen) {
    chosen = 0;
    uint32_t low = 1, high = source.size;
    constexpr unsigned continuation_mask = 0xc0, continuation = 0x80;
    while (low <= high) {
        const uint32_t middle = low + (high - low) / 2U;
        uint32_t take = middle;
        while (take && take < source.size &&
               (static_cast<unsigned char>(source.data[take]) & continuation_mask) ==
                   continuation) {
            --take;
        }
        const auto status = take ? make(take) : AYNI_CAPACITY_EXCEEDED;
        if (!take || status == AYNI_OK) {
            chosen = std::max(chosen, take);
            low = middle + 1U;
        } else if (status == AYNI_CAPACITY_EXCEEDED) {
            high = middle - 1U;
        } else {
            return status;
        }
    }
    return chosen ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
// Local errors are matching tool responses. A successful requested=true means
// Core copied a concrete business Call; no local response may then be fabricated.
ayni_status_t native_function(const ayni_tool_call_v1 &proposal, const ToolLoopConfig &config,
                              const ayni_task_snapshot_v1 &snapshot, const Turn &turn,
                              ayni_string_v1 step, ayni_policy_output_v1 &output, char *buffer,
                              uint32_t capacity, ToolBudget budget, ayni_string_v1 &response,
                              bool &requested);
// Serialize original known result proof and outcome, retaining UNKNOWN as a hold.
ayni_status_t native_result(const ayni_task_call_result_v1 &result, char *buffer, uint32_t capacity,
                            ayni_string_v1 &response);
// Normal harness errors never claim a failed or unknown business action.
ayni_status_t native_error(ayni_status_t status, std::string_view message, char *buffer,
                           uint32_t capacity, ayni_string_v1 &response);
// Chunk only the result body; the final reply keeps the original action facts.
ayni_status_t summarize_result(Turn &, const ToolLoopConfig &, const ayni_task_snapshot_v1 &,
                               ayni_policy_output_v1 &, char *, bool &requested);
// Only an asynchronous archive wait marks DEFER; service failure statuses remain failures.
ayni_status_t archive_result(const ToolLoopConfig &, const ayni_task_snapshot_v1 &,
                             const ConversationRecord &, const ayni_task_call_result_v1 &,
                             ArchivedResult &, ayni_policy_output_v1 &);
} // namespace ayni::strategies::detail
#endif
