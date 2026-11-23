#include "harness.h"
#include <algorithm>
#include <ayni/call_result_json.hpp>
#include <ayni/values.hpp>
#include <charconv>
#include <limits>
namespace ayni::strategies::detail {
namespace {
bool named(ayni_string_v1 text, std::string_view name) {
    return equal(text, view(name));
}
bool done(json::Writer &out, char *buffer, ayni_string_v1 &response) {
    size_t size = 0;
    if (!out.finish(size) || size > UINT32_MAX) {
        return false;
    }
    response = {buffer, static_cast<uint32_t>(size)};
    return true;
}
// Model cursors survive ordinary work commits. Exact current visibility and
// context version are rechecked; the offset itself confers no authority.
struct Cursor {
    static constexpr uint32_t word_count = 5U + AYNI_CAPABILITY_DISCLOSURE_WORDS;
    std::array<uint64_t, word_count> words{};
    bool read(ayni_string_v1 text) {
        const char *at = text.data, *end = text.data + text.size;
        for (auto &word : words) {
            const auto parsed = std::from_chars(at, end, word, 16);
            if (parsed.ec != std::errc{} || parsed.ptr == at) {
                return false;
            }
            at = parsed.ptr;
            if (&word != &words.back()) {
                if (at == end || *at++ != '.') {
                    return false;
                }
            }
        }
        return at == end;
    }
    void write(json::Writer &out) const {
        constexpr uint32_t hex_digits = 16;
        char bytes[word_count * (hex_digits + 1U)];
        char *at = bytes;
        for (uint32_t i = 0; i < words.size(); ++i) {
            if (i) {
                *at++ = '.';
            }
            const auto result = std::to_chars(at, bytes + sizeof(bytes), words[i], 16);
            at = result.ptr;
        }
        out.string(std::string_view(bytes, static_cast<size_t>(at - bytes)));
    }
};
ayni_status_t list(const NativeArguments &args, const ayni_task_snapshot_v1 &snapshot,
                   const Turn &turn, char *buffer, uint32_t capacity, ToolBudget budget,
                   ToolReply &reply) {
    auto &response = reply.text;
    if (!snapshot.list_capabilities || !snapshot.capability_context) {
        return AYNI_UNSUPPORTED;
    }
    uint32_t limit = AYNI_CAPABILITY_PAGE_MAX;
    if (args.positions[1] != UINT32_MAX &&
        (!args.integer(1, limit) || !limit || limit > AYNI_CAPABILITY_PAGE_MAX)) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_capability_context_v1 context{};
    auto status = snapshot.capability_context(snapshot.call_context, nullptr, &context);
    if (status != AYNI_OK) {
        return status;
    }
    Cursor expected;
    expected.words[0] = 1;
    expected.words[1] = context.boot_epoch;
    expected.words[2] = snapshot.task_id;
    expected.words[3] = turn.record.context_version;
    for (uint32_t i = 0; i < AYNI_CAPABILITY_DISCLOSURE_WORDS; ++i) {
        expected.words[5U + i] = context.visible[i];
    }
    uint32_t offset = 0;
    if (args.positions[0] != UINT32_MAX) {
        ayni_string_v1 token{};
        Cursor previous;
        if (!args.string(0, token)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (token.size) {
            if (!previous.read(token) || previous.words[4] > AYNI_GOAL_TYPE_TOOL_COUNT_MAX) {
                return AYNI_INVALID_ARGUMENT;
            }
            offset = static_cast<uint32_t>(previous.words[4]);
            previous.words[4] = 0;
            if (previous.words != expected.words) {
                return AYNI_REVISION_CONFLICT;
            }
        }
    }
    ayni_capability_cursor_v1 cursor{}, next{};
    std::array<ayni_capability_info_v1, AYNI_CAPABILITY_PAGE_MAX> items{};
    uint32_t count = 0, skipped = 0;
    while (skipped < offset) {
        const uint32_t take = std::min<uint32_t>(offset - skipped, AYNI_CAPABILITY_PAGE_MAX);
        status = snapshot.list_capabilities(snapshot.call_context, &cursor, items.data(), take,
                                            &count, &next);
        if (status != AYNI_OK) {
            return status;
        }
        if (count != take || (!next.boot_epoch && skipped + count < offset)) {
            return AYNI_REVISION_CONFLICT;
        }
        skipped += count;
        cursor = next;
        if (!cursor.boot_epoch) {
            return AYNI_REVISION_CONFLICT;
        }
    }
    status = snapshot.list_capabilities(snapshot.call_context, &cursor, items.data(), limit, &count,
                                        &next);
    if (status != AYNI_OK) {
        return status;
    }
    const auto emit = [&](json::Writer &writer, uint32_t take) {
        writer.raw("{\"capabilities\":[");
        for (uint32_t i = 0; i < take; ++i) {
            if (i) {
                writer.raw(",");
            }
            const auto &item = items[i];
            writer.raw("{\"binding_id\":");
            writer.string(item.binding_id);
            writer.raw(",\"binding_version\":");
            writer.unsigned_number(item.binding_version);
            writer.raw(",\"capability_id\":");
            writer.string(item.capability_id);
            writer.raw(",\"capability_version\":");
            writer.unsigned_number(item.capability_version);
            writer.raw(",\"form\":");
            writer.number(item.form);
            writer.raw(",\"cancel_mode\":");
            writer.number(item.cancel_mode);
            writer.raw(",\"role\":");
            writer.string(item.role == AYNI_TOOL_ROLE_SHELL ? "shell" : "capability");
            writer.raw(",\"summary\":");
            writer.string(item.summary);
            writer.raw("}");
        }
        writer.raw("],\"next_cursor\":");
        if (take < count || next.boot_epoch) {
            expected.words[4] = offset + take;
            expected.write(writer);
        } else {
            writer.raw("null");
        }
        writer.raw("}");
    };
    uint32_t chosen = count;
    for (;;) {
        json::Writer candidate(buffer, capacity);
        emit(candidate, chosen);
        if (done(candidate, buffer, response)) {
            const auto measured = budget.measure_estimate(response);
            if (measured.tokens <= budget.tokens) {
                reply.measured = measured;
                return AYNI_OK;
            }
        }
        if (!chosen) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        --chosen;
        if (!chosen && count) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
}
ayni_status_t inspect(Workspace &workspace, const ayni_capability_detail_v1 &detail,
                      json::Writer &out) {
    out.raw("{\"binding_id\":");
    out.string(detail.info.binding_id);
    out.raw(",\"binding_version\":");
    out.unsigned_number(detail.info.binding_version);
    out.raw(",\"form\":");
    out.number(detail.info.form);
    out.raw(",\"cancel_mode\":");
    out.number(detail.info.cancel_mode);
    out.raw(",\"summary\":");
    out.string(detail.info.summary);
    out.raw(",\"purpose\":");
    out.string(detail.usage.purpose);
    out.raw(",\"input_usage\":");
    out.string(detail.usage.text_usage);
    out.raw(",\"result_usage\":");
    out.string(detail.usage.result_usage);
    out.raw(",\"examples\":");
    out.string(detail.usage.examples);
    return ::ayni::detail::with_workspace(
        workspace, ayni_business_workspace_requirements, [&](const ayni_workspace_v1 *region) {
            return ayni_business_contract_read(
                region, detail.original_domain, detail.business_contract, &out,
                [](void *opaque, const ayni_business_contract_v1 *business) {
                    auto &writer = *static_cast<json::Writer *>(opaque);
                    writer.raw(",\"input_schema\":");
                    json::write_schema(writer, business->input);
                    writer.raw(",\"result_schema\":");
                    json::write_schema(writer, business->result);
                    writer.raw("}");
                    return AYNI_OK;
                });
        });
}
// Copy/rebase the selected source subtree. The same original Schema normalizer
// admits defaults, numbers and object closure before the ordinary Call boundary.
ayni_status_t invoke(Workspace &workspace, const NativeArguments &args, uint32_t argument,
                     const ayni_capability_detail_v1 &detail, ayni_string_v1 step,
                     ayni_policy_output_v1 &output, uint32_t mode) {
    auto source_lease =
        workspace.make<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>>();
    if (!source_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &source = *source_lease;
    const uint32_t begin = args.positions[argument];
    const uint32_t count = args.tree.nodes[begin].subtree_end - begin;
    for (uint32_t i = 0; i < count; ++i) {
        source[i] = args.tree.nodes[begin + i];
        source[i].subtree_end -= begin;
    }
    source[0].name = {};
    struct State {
        ayni_arguments_v1 source;
        const ayni_capability_detail_v1 &detail;
        ayni_string_v1 step;
        ayni_policy_output_v1 &output;
        uint32_t mode;
        Workspace &workspace;
    } inspection{{source.data(), count}, detail, step, output, mode, workspace};
    return ::ayni::detail::with_workspace(
        workspace, ayni_business_workspace_requirements, [&](const ayni_workspace_v1 *region) {
            return ayni_business_contract_read(
                region, detail.original_domain, detail.business_contract, &inspection,
                [](void *opaque, const ayni_business_contract_v1 *business) {
                    auto &state = *static_cast<State *>(opaque);
                    if (state.mode == AYNI_CALL_MODE_FOREGROUND &&
                        state.detail.info.form == AYNI_CALL_SUBSCRIPTION) {
                        return AYNI_INVALID_ARGUMENT; // Subscription requires an explicit
                                                      // model-managed start.
                    }
                    auto nodes_lease =
                        state.workspace
                            .make<std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX>>();
                    if (!nodes_lease) {
                        return AYNI_CAPACITY_EXCEEDED;
                    }
                    auto &nodes = *nodes_lease;
                    auto bytes_lease =
                        state.workspace.make<std::array<char, AYNI_VALUE_BYTES_MAX>>();
                    if (!bytes_lease) {
                        return AYNI_CAPACITY_EXCEEDED;
                    }
                    auto &bytes = *bytes_lease;
                    ayni_value_buffer_v1 value{
                        nodes.data(), static_cast<uint32_t>(nodes.size()), 0,
                        bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
                    const auto status = ::ayni::detail::with_workspace(
                        state.workspace, ayni_value_workspace_requirements,
                        [&](const ayni_workspace_v1 *value_region) {
                            return ayni_arguments_normalize(value_region, &business->input,
                                                            &state.source, &value);
                        });
                    if (status != AYNI_OK) {
                        return status;
                    }
                    Call call;
                    call.mode = state.mode;
                    call.binding_id = state.detail.info.binding_id;
                    call.binding_version = state.detail.info.binding_version;
                    call.input = ayni_value_v1{nodes.data(), value.node_count};
                    return request(call, state.step, {}, state.output);
                });
        });
}
} // namespace
ayni_status_t native_error(ayni_status_t status, std::string_view message, ReplyBuffer &scratch,
                           ayni_string_v1 &response) {
    auto *buffer = scratch.data();
    const auto capacity = scratch.capacity;
    json::Writer out(buffer, capacity);
    out.raw("{\"error\":{\"status\":");
    out.number(status);
    out.raw(",\"message\":");
    out.string(message);
    out.raw("}}");
    return done(out, buffer, response) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t native_function(Workspace &workspace, const ayni_tool_call_v1 &proposal,
                              const ToolLoopConfig &config, const ayni_task_snapshot_v1 &snapshot,
                              Turn &turn, ayni_string_v1 step, ayni_policy_output_v1 &output,
                              ReplyBuffer &scratch, ToolBudget budget, ToolReply &reply,
                              bool &requested) {
    auto &response = reply.text;
    requested = false;
    NativeArguments args;
    const bool capability = named(proposal.function_name, model::call_function);
    ayni_string_v1 operation{};
    if (capability) {
        if (!read_capability_arguments(proposal, args)) {
            return native_error(AYNI_INVALID_ARGUMENT, "Invalid operation envelope.", scratch,
                                response);
        }
        if (args.positions[operation_field] != UINT32_MAX &&
            (!args.string(operation_field, operation) ||
             (!named(operation, "invoke") && !named(operation, "start")))) {
            return native_operation(workspace, args, operation, config, snapshot, step, output,
                                    scratch, reply, requested);
        }
    }
    ayni_status_t status = AYNI_NOT_FOUND;
    if (named(proposal.function_name, model::list_function)) {
        status = args.read(proposal, {"cursor", "limit"})
                     ? list(args, snapshot, turn, scratch.data(), scratch.capacity, budget, reply)
                     : AYNI_INVALID_ARGUMENT;
        if (status == AYNI_OK) {
            return AYNI_OK;
        }
    } else if (named(proposal.function_name, model::inspect_function) || capability ||
               named(proposal.function_name, model::shell_function)) {
        const bool shell = named(proposal.function_name, model::shell_function);
        const bool call = capability || shell;
        ayni_string_v1 binding{};
        uint32_t version = 0;
        const uint32_t mode =
            named(operation, "start") ? AYNI_CALL_MODE_MANAGED : AYNI_CALL_MODE_FOREGROUND;
        bool valid = false;
        if (shell) {
            binding = config.shell.binding_id;
            version = config.shell.binding_version;
            valid = args.read(proposal, {"command"}) && args.count == 1U;
        } else {
            valid = (capability || args.read(proposal, {"binding_id", "binding_version"})) &&
                    args.string(binding_field, binding) && valid_id(binding) &&
                    args.integer(version_field, version) && version;
            if (capability) {
                constexpr uint32_t invoke_fields = 3;
                valid = valid && args.positions[arguments_field] != UINT32_MAX &&
                        args.count == invoke_fields + (operation.size ? 1U : 0U);
            } else {
                valid = valid && args.count == 2U;
            }
        }
        ayni_capability_detail_v1 detail{};
        detail.struct_size = sizeof(detail);
        status = shell && !binding.size ? AYNI_NOT_FOUND
                 : valid                ? (snapshot.inspect_capability
                                               ? snapshot.inspect_capability(snapshot.call_context, binding,
                                                                             version, &detail)
                                               : AYNI_UNSUPPORTED)
                                        : AYNI_INVALID_ARGUMENT;
        if (status == AYNI_OK && shell && detail.info.role != AYNI_TOOL_ROLE_SHELL) {
            status = AYNI_NOT_FOUND;
        }
        if (status == AYNI_OK && !call) {
            auto *buffer = scratch.data();
            json::Writer out(buffer, scratch.capacity);
            status = inspect(workspace, detail, out);
            if (status == AYNI_OK && done(out, buffer, response)) {
                const auto measured = budget.measure_estimate(response);
                if (measured.tokens <= budget.tokens) {
                    reply.measured = measured;
                    return AYNI_OK;
                }
            }
        } else if (status == AYNI_OK) {
            // Shell's complete command object is itself the business input.
            if (shell) {
                args.positions[arguments_field] = 0;
            }
            status = invoke(workspace, args, arguments_field, detail, step, output, mode);
            if (status == AYNI_OK) {
                requested = true;
                return AYNI_OK;
            }
        }
    }
    if (status == AYNI_OK) {
        status = AYNI_CAPACITY_EXCEEDED;
    }
    return native_error(status,
                        "Function unavailable, invalid arguments, stale cursor, or explicit "
                        "resource limit. No business Call was accepted.",
                        scratch, response);
}
ayni_status_t native_result(Workspace &workspace, const ayni_task_call_result_v1 &result,
                            ReplyBuffer &scratch, ayni_string_v1 &response) {
    if (!result.outcome) {
        return AYNI_BUSY;
    }
    if (result.outcome == AYNI_CALL_ABANDONED || result.status == AYNI_RESULT_UNKNOWN ||
        result.effect == AYNI_CALL_EFFECT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    auto *buffer = scratch.data();
    json::Writer out(buffer, scratch.capacity);
    const auto status = ::ayni::detail::with_workspace(
        workspace, json::call_result_workspace_requirements, [&](const ayni_workspace_v1 *region) {
            return json::write_call_result(region, out, result);
        });
    if (status != AYNI_OK) {
        return status;
    }
    return done(out, buffer, response) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

} // namespace ayni::strategies::detail
