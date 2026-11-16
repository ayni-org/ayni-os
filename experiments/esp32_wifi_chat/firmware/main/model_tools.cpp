#include "model_tools.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <ayni/values_json.hpp>
#include <cstring>
namespace wifichat {
ayni_status_t ModelTools::decode(ayni_bytes_v1 bytes, const ayni_workspace_v1 &workspace) {
    count_ = 0;
    assistant_ = {};
    if (!bytes.data || bytes.size > wire_.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::memcpy(wire_.data(), bytes.data, bytes.size);
    const ayni_bytes_v1 owned{wire_.data(), bytes.size};
    uint32_t count = 0;
    const auto status = ayni_tool_calls_read(
        &workspace, owned, this,
        [](void *context, uint32_t index, const ayni_tool_call_v1 *call) {
            auto &self = *static_cast<ModelTools *>(context);
            if (index >= self.calls_.size() ||
                (call->input_kind == AYNI_TOOL_INPUT_ARGUMENTS &&
                 call->input.arguments.node_count > nodes_per_tool)) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            self.calls_[index] = *call;
            if (call->input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
                const auto &args = call->input.arguments;
                std::copy_n(args.nodes, args.node_count, self.nodes_[index].data());
                self.calls_[index].input.arguments.nodes = self.nodes_[index].data();
            }
            return AYNI_OK;
        },
        &count, &assistant_);
    if (status == AYNI_OK) {
        count_ = count;
    }
    return status;
}
ayni_status_t ModelTools::outline(char *buffer, size_t capacity, size_t &written) const {
    written = 0;
    ayni::json::Writer out(buffer, capacity);
    out.raw("[");
    for (uint32_t at = 0; at < count_; ++at) {
        if (at) {
            out.raw(",");
        }
        const auto &call = calls_[at];
        const std::string_view function(call.function_name.data, call.function_name.size);
        bool known = false;
        for (const auto &spec : ayni::model::functions) {
            known = known || function == spec.name;
        }
        out.raw("{\"function\":");
        out.string(known ? function : std::string_view{});
        out.raw(",\"binding\":");
        ayni_string_v1 binding{};
        std::string_view operation;
        const bool has_binding =
            function == ayni::model::call_function || function == ayni::model::inspect_function;
        if (has_binding && call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
            const auto &input = call.input.arguments;
            if (input.node_count && input.nodes[0].kind == AYNI_ARGUMENT_OBJECT) {
                operation = function == ayni::model::call_function ? "invoke" : "";
                // Inspect only protocol root fields, skipping each business subtree.
                // SDK decoding already validated this tree's subtree_end boundaries.
                for (uint32_t node = 1; node < input.node_count;
                     node = input.nodes[node].subtree_end) {
                    const auto &value = input.nodes[node];
                    if (function == ayni::model::call_function && value.name.data &&
                        std::string_view(value.name.data, value.name.size) == "operation") {
                        operation = {};
                        if (value.kind == AYNI_ARGUMENT_STRING) {
                            for (const auto mode :
                                 {"invoke", "start", "read", "wait", "cancel", "close"}) {
                                if (std::string_view(value.value.text.data,
                                                     value.value.text.size) == mode) {
                                    operation = mode; // Fixed protocol enum, never arbitrary text.
                                    break;
                                }
                            }
                        }
                        continue;
                    }
                    if (value.kind != AYNI_ARGUMENT_STRING || !value.name.data ||
                        std::string_view(value.name.data, value.name.size) != "binding_id") {
                        continue;
                    }
                    ayni_value_node_v1 identifier{};
                    identifier.kind = AYNI_VALUE_IDENTIFIER;
                    identifier.subtree_end = 1;
                    identifier.value.text = value.value.text;
                    const ayni_value_v1 view{&identifier, 1};
                    // Reuse the public identifier validator, rather than a parallel grammar.
                    if (ayni_value_validate(&view) == AYNI_OK) {
                        binding = value.value.text;
                    }
                }
            }
        }
        out.string(binding.data ? std::string_view(binding.data, binding.size)
                                : std::string_view{});
        out.raw(",\"operation\":");
        out.string(operation);
        out.raw("}");
    }
    out.raw("]");
    return out.finish(written) ? AYNI_OK : AYNI_BUFFER_TOO_SMALL;
}
} // namespace wifichat
