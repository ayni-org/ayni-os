// Native function routing owns local directory/result rendering, never Kernel state.
#ifndef AYNI_STRATEGIES_HARNESS_H
#define AYNI_STRATEGIES_HARNESS_H
#include "support.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <ayni/operation_json.hpp>
#include <charconv>
namespace ayni::strategies::detail {
inline constexpr uint32_t native_argument_capacity = 7; // All fixed operation-envelope fields.
// A model frame Writer appending to text, which grows on demand (ADR-0095).
inline model::Writer growing(std::string &text) {
    const model::Writer::Grow grow = [](void *context, uint32_t size) -> char * {
        auto &out = *static_cast<std::string *>(context);
        out.resize(size);
        return out.data();
    };
    return {&text, grow, static_cast<uint32_t>(text.size())};
}
// Native objects are closed, including duplicate keys. Business arguments retain
// their own names until the selected original Schema resolves them.
struct NativeArguments {
    ayni_arguments_v1 tree{};
    std::array<uint32_t, native_argument_capacity> positions{};
    uint32_t count = 0;
    bool read(const ayni_tool_call_v1 &call, std::initializer_list<std::string_view> names) {
        if (call.input_kind != AYNI_TOOL_INPUT_ARGUMENTS) {
            return false;
        }
        count = 0;
        if (names.size() > positions.size()) {
            return false;
        }
        tree = call.input.arguments;
        if (ayni_arguments_validate(&tree) != AYNI_OK ||
            tree.nodes[0].kind != AYNI_ARGUMENT_OBJECT) {
            return false;
        }
        positions.fill(UINT32_MAX);
        for (uint32_t at = 1; at < tree.node_count; at = tree.nodes[at].subtree_end) {
            uint32_t index = 0;
            for (auto name : names) {
                if (equal(tree.nodes[at].name, view(name))) {
                    break;
                }
                ++index;
            }
            if (index == names.size() || positions[index] != UINT32_MAX) {
                return false;
            }
            positions[index] = at;
            ++count;
        }
        return true;
    }
    bool string(uint32_t field, ayni_string_v1 &out) const {
        if (positions[field] == UINT32_MAX) {
            return false;
        }
        const auto &node = tree.nodes[positions[field]];
        if (node.kind != AYNI_ARGUMENT_STRING) {
            return false;
        }
        out = node.value.text;
        return true;
    }
    bool integer(uint32_t field, uint32_t &out) const {
        if (positions[field] == UINT32_MAX) {
            return false;
        }
        const auto &node = tree.nodes[positions[field]];
        if (node.kind != AYNI_ARGUMENT_NUMBER || !node.value.text.size) {
            return false;
        }
        auto text = node.value.text;
        auto parsed = std::from_chars(text.data, text.data + text.size, out);
        return parsed.ec == std::errc{} && parsed.ptr == text.data + text.size;
    }
};

struct ToolBudget {
    uint64_t tokens = UINT64_MAX; // Estimated remaining input tokens; MAX disables token limiting.
    ayni_string_v1 protocol_id{}; // Current opaque ID, counted with the matching reply envelope.
    model::Projection projection{}; // Same wire envelope as this request.
    bool fits(ayni_string_v1 reply) const {
        return model::estimate_tool_reply(projection, protocol_id, reply) <= tokens;
    }
};
// Current pending tool reply budget, including the real API projection, definitions and
// protected continuation. Shared by raw results and completed result summaries.
ayni_status_t current_tool_budget(const ToolLoopConfig &, const Turn &, ayni_string_v1,
                                  ToolBudget &);
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
// Copies the current Task-visible registered definitions before snapshot borrows expire.
ayni_status_t preload_tools(Workspace &, const ayni_task_snapshot_v1 &, std::string &,
                            std::map<std::string, std::string> &);
// Local errors are matching tool responses. A successful requested=true means
// Core copied a concrete business Call; no local response may then be fabricated.
ayni_status_t native_function(Workspace &workspace, const ayni_tool_call_v1 &proposal,
                              const ToolLoopConfig &config, const ayni_task_snapshot_v1 &snapshot,
                              Turn &turn, ayni_string_v1 step, ayni_policy_output_v1 &output,
                              char *buffer, uint32_t capacity, ToolBudget budget,
                              ayni_string_v1 &response, bool &requested);
// Handles read/wait/cancel/close; start/invoke remain ordinary normalized business admission.
ayni_status_t native_operation(Workspace &workspace, const ayni_tool_call_v1 &,
                               const ToolLoopConfig &, const ayni_task_snapshot_v1 &, Turn &,
                               ayni_string_v1, ayni_policy_output_v1 &, char *, uint32_t,
                               ToolBudget, ayni_string_v1 &, bool &, bool &);
// Render an original operation and optionally its known terminal result.
ayni_status_t operation_result(Workspace &workspace, const ayni_operation_info_v1 &,
                               const ToolLoopConfig &, const ayni_task_snapshot_v1 &, Turn &,
                               ayni_policy_output_v1 &, char *, uint32_t, ToolBudget,
                               ayni_string_v1 &, ayni_string_v1 protocol_id,
                               ayni_status_t = AYNI_OK);
// Stable tool processing steps are distinct from the target's business completion.
bool step_committed(const ayni_task_snapshot_v1 &, ayni_string_v1);
// A control proposal's original target supplies its terminal body for the common summary path.
ayni_status_t operation_source(const ayni_tool_call_v1 &, const ayni_task_snapshot_v1 &,
                               ayni_task_call_result_v1 &);
// Serialize original known result proof and outcome, retaining UNKNOWN as a hold.
ayni_status_t native_result(Workspace &workspace, const ayni_task_call_result_v1 &result,
                            char *buffer, uint32_t capacity, ayni_string_v1 &response);
// Normal harness errors never claim a failed or unknown business action.
ayni_status_t native_error(ayni_status_t status, std::string_view message, char *buffer,
                           uint32_t capacity, ayni_string_v1 &response);
// Chunk only the result body; the final reply keeps the original action facts.
ayni_status_t summarize_result(Workspace &workspace, Turn &, const ToolLoopConfig &,
                               const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &,
                               bool &requested);
// Rendering room for a reply of the given tokens (tool_loop.cpp).
uint32_t reply_bytes(uint64_t tokens);
// Both unary and managed replies retain the same original value/proof representation.
ayni_status_t retain_tool_result(const ToolLoopConfig &, Turn &, ayni_string_v1 protocol_id,
                                 const ayni_task_call_result_v1 &, ayni_string_v1 presentation,
                                 ayni_policy_output_v1 &);
// Preserve original result/proofs in the common body graph before requesting summaries.
ayni_status_t begin_result_summary(const ToolLoopConfig &, Turn &, const ayni_task_call_result_v1 &,
                                   ayni_policy_output_v1 &);
} // namespace ayni::strategies::detail
#endif
