// Experiment-only bounded harness: discovery + registered screen/speaker tools.
// It consumes the public Call ledger and stores complete tool groups in Core checkpoints.
#include "chat.h"
#include <ayni/directory.h>
#include <ayni/model_messages.hpp>
#include <ayni/values.hpp>
#include <charconv>
#include <cstdio>
#include <cstring>
namespace usbchat {
namespace {
constexpr uint32_t state_version = 1, state_header = 12, maximum_rounds = 12;
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && !std::memcmp(a.data, b, a.size);
}
template <class T> T sized() {
    T v{};
    v.struct_size = sizeof(v);
    return v;
}
const ayni_argument_node_v1 *field(ayni_arguments_v1 a, uint32_t root, const char *name) {
    if (!a.nodes || root >= a.node_count || a.nodes[root].kind != AYNI_ARGUMENT_OBJECT) {
        return nullptr;
    }
    for (uint32_t i = root + 1; i < a.nodes[root].subtree_end; i = a.nodes[i].subtree_end) {
        if (same(a.nodes[i].name, name)) {
            return &a.nodes[i];
        }
    }
    return nullptr;
}
bool only(ayni_arguments_v1 a, const char *names) {
    if (!a.node_count || a.nodes[0].kind != AYNI_ARGUMENT_OBJECT) {
        return false;
    }
    for (uint32_t i = 1; i < a.node_count; i = a.nodes[i].subtree_end) {
        const std::string_view name(a.nodes[i].name.data, a.nodes[i].name.size);
        const std::string_view accepted(names);
        const auto at = accepted.find(name);
        if (at == std::string_view::npos || at == 0 || accepted[at - 1] != '|' ||
            at + name.size() >= accepted.size() || accepted[at + name.size()] != '|') {
            return false;
        }
    }
    return true;
}
// Opaque cursor encoding belongs to this adapter; Core still validates every field.
constexpr size_t cursor_bytes = 48, cursor_text_bytes = cursor_bytes * 2;
bool read_cursor(ayni_string_v1 token, ayni_capability_cursor_v1 &out) {
    if (!token.size) {
        out = {};
        return true;
    }
    if (token.size != cursor_text_bytes) {
        return false;
    }
    uint8_t bytes[cursor_bytes]{};
    for (size_t i = 0; i < cursor_bytes; ++i) {
        unsigned value = 0;
        const auto parsed = std::from_chars(token.data + i * 2, token.data + i * 2 + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data + i * 2 + 2) {
            return false;
        }
        bytes[i] = value;
    }
    out = {get64(bytes),      get64(bytes + 8),  get64(bytes + 16), get64(bytes + 24),
           get64(bytes + 32), get32(bytes + 40), get32(bytes + 44)};
    return true;
}
void write_cursor(ayni::json::Writer &out, const ayni_capability_cursor_v1 &cursor) {
    if (!cursor.boot_epoch) {
        out.string(text(""));
        return;
    }
    uint8_t bytes[cursor_bytes]{};
    char token[cursor_text_bytes]{};
    put64(bytes, cursor.boot_epoch);
    put64(bytes + 8, cursor.task_id);
    put64(bytes + 16, cursor.commit_seq);
    put64(bytes + 24, cursor.durable_seq);
    put64(bytes + 32, cursor.valid_until_us);
    put32(bytes + 40, cursor.gate_count);
    put32(bytes + 44, cursor.position);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < cursor_bytes; ++i) {
        token[i * 2] = digits[bytes[i] >> 4U];
        token[i * 2 + 1] = digits[bytes[i] & 15U];
    }
    out.string(ayni_string_v1{token, uint32_t(cursor_text_bytes)});
}
ayni_status_t report(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out, uint32_t kind) {
    if (!out.intents || !out.intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out.intents[0] = sized<ayni_policy_intent_v1>();
    out.intents[0].kind = kind;
    out.intents[0].expected_revision = s.revision;
    out.intent_count = 1;
    return AYNI_OK;
}
} // namespace
ayni_schema_v1 Chat::tool_schema(ayni_string_v1 binding) const {
    if (same(binding, "screen.show_text")) {
        return screen_business_.input;
    }
    const auto motor = motor_.schema(binding);
    return motor.nodes ? motor : speaker_.schema(binding);
}
ayni_status_t Chat::decide(void *ctx, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out) {
    auto &self = *static_cast<Chat *>(ctx);
    // This interactive deployment starts a fresh session: restored work must
    // wait for owner cancellation, never propose new model or device calls.
    if (!self.announced_ || self.closing_ != Closing::open) {
        return AYNI_NOT_READY;
    }
    uint32_t round = 0;
    ayni_string_v1 messages{reinterpret_cast<const char *>(s->goal_parameters.data),
                            s->goal_parameters.size};
    if (s->checkpoint.state.size) {
        if (s->checkpoint.state.size < state_header ||
            get32(s->checkpoint.state.data) != state_version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
        round = get32(s->checkpoint.state.data + 4);
        messages = {reinterpret_cast<const char *>(s->checkpoint.state.data + state_header),
                    s->checkpoint.state.size - state_header};
    }
    if (round >= maximum_rounds) {
        return report(*s, *out, AYNI_POLICY_REPORT_FAILURE);
    }
    char model_key[32], tool_key[32], progress_key[32];
    std::snprintf(model_key, sizeof(model_key), "model.%lu", static_cast<unsigned long>(round));
    // Keep the existing checkpoint key namespace across this experiment upgrade.
    std::snprintf(tool_key, sizeof(tool_key), "screen.%lu", static_cast<unsigned long>(round));
    std::snprintf(progress_key, sizeof(progress_key), "native.%lu",
                  static_cast<unsigned long>(round));
    auto model = sized<ayni_task_call_result_v1>();
    auto rc = s->call_result(s->call_context, text(model_key), &model);
    if (rc == AYNI_NOT_FOUND) {
        ayni::ValueBuilder<3> value;
        value.object();
        value.identifier(AYNI_MODEL_DESTINATION_FIELD_ID, self.destination_);
        value.text(AYNI_MODEL_BODY_FIELD_ID, messages);
        value.end();
        auto request = sized<ayni_call_request_v1>();
        request.kind = AYNI_POLICY_REQUEST_MODEL;
        request.binding_id = text("usb.model");
        request.binding_version = 1;
        request.step_key = text(model_key);
        rc = value.finish(request.value);
        return rc == AYNI_OK ? out->request_call(out->call_context, &request) : rc;
    }
    if (rc != AYNI_OK) {
        return rc;
    }
    if (model.outcome != AYNI_CALL_COMPLETED || model.status != AYNI_OK) {
        return report(*s, *out, AYNI_POLICY_REPORT_FAILURE);
    }
    if (!model.tool_call_count) {
        if (model.text.size > answer_bytes ||
            model.text.size + state_header > out->checkpoint_capacity) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        put32(out->checkpoint, state_version);
        put32(out->checkpoint + 4, round);
        put32(out->checkpoint + 8, 1);
        std::memcpy(out->checkpoint + state_header, model.text.data, model.text.size);
        out->checkpoint_size = state_header + model.text.size;
        return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
    }
    if (model.tool_call_count > AYNI_CALL_TOOL_COUNT_MAX) {
        return report(*s, *out, AYNI_POLICY_REPORT_FAILURE);
    }
    // Keep the assistant's complete group and original IDs. The Call ledger owns
    // each effect; reconstructing read-only directory replies never replays it.
    ayni::model::Writer history(self.policy_messages_.data(), self.policy_messages_.size());
    history.raw(messages);
    history.calls(model.tool_call_count);
    auto proposal = sized<ayni_task_tool_call_v1>();
    for (uint32_t at = 0; at < model.tool_call_count; ++at) {
        rc = s->call_tool(s->call_context, text(model_key), at, &proposal);
        if (rc != AYNI_OK) {
            return rc;
        }
        const auto &call = proposal.call;
        if (call.input_kind != AYNI_TOOL_INPUT_ARGUMENTS) {
            return report(*s, *out, AYNI_POLICY_REPORT_FAILURE);
        }
        ayni::json::Writer arguments(self.arguments_json_.data(), self.arguments_json_.size());
        rc = ayni::json::write_arguments(arguments, call.input.arguments);
        size_t args_bytes = 0;
        if (rc != AYNI_OK || !arguments.finish(args_bytes)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        history.function({call.protocol_id,
                          call.function_name,
                          {self.arguments_json_.data(), uint32_t(args_bytes)}});
    }
    for (uint32_t at = 0; at < model.tool_call_count; ++at) {
        rc = s->call_tool(s->call_context, text(model_key), at, &proposal);
        if (rc != AYNI_OK) {
            return rc;
        }
        // Index zero preserves the prior single-tool step identity on old Tasks.
        if (at) {
            std::snprintf(tool_key, sizeof(tool_key), "screen.%lu.%lu",
                          static_cast<unsigned long>(round), static_cast<unsigned long>(at));
        }
        bool requested = false;
        ayni_string_v1 response{};
        rc = self.reply_to_tool(s, out, proposal.call, text(tool_key), requested, response);
        if (rc != AYNI_OK || requested) {
            return rc;
        }
        history.tool(proposal.call.protocol_id, response);
    }
    ayni_string_v1 framed{};
    if (!history.finish(framed) || framed.size + state_header > out->checkpoint_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    put32(out->checkpoint, state_version);
    put32(out->checkpoint + 4, round + 1);
    put32(out->checkpoint + 8, 0);
    std::memcpy(out->checkpoint + state_header, framed.data, framed.size);
    out->checkpoint_size = framed.size + state_header;
    rc = report(*s, *out, AYNI_POLICY_REPORT_PROGRESS);
    if (rc == AYNI_OK) {
        out->intents[0].step_key_size = std::strlen(progress_key);
        std::memcpy(out->intents[0].step_key, progress_key, out->intents[0].step_key_size);
    }
    return rc;
}
ayni_status_t Chat::reply_to_tool(const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out,
                                  const ayni_tool_call_v1 &call, ayni_string_v1 step_key,
                                  bool &intent_submitted, ayni_string_v1 &response) {
    auto &self = *this;
    intent_submitted = false;
    response = {};
    auto rc = AYNI_OK;
    const auto args = call.input.arguments;
    ayni::json::Writer reply(self.tool_reply_.data(), self.tool_reply_.size());
    if (same(call.function_name, "ayni_list_capabilities") && only(args, "|cursor||limit|")) {
        const auto *token = field(args, 0, "cursor"), *requested = field(args, 0, "limit");
        uint32_t limit = AYNI_CAPABILITY_PAGE_MAX;
        bool valid = true;
        ayni_capability_cursor_v1 cursor{}, next{};
        if (token) {
            valid = token->kind == AYNI_ARGUMENT_STRING && read_cursor(token->value.text, cursor);
        }
        if (requested) {
            if (requested->kind != AYNI_ARGUMENT_NUMBER) {
                valid = false;
            } else {
                const auto parsed =
                    std::from_chars(requested->value.text.data,
                                    requested->value.text.data + requested->value.text.size, limit);
                valid = valid && parsed.ec == std::errc{} &&
                        parsed.ptr == requested->value.text.data + requested->value.text.size &&
                        limit > 0 && limit <= AYNI_CAPABILITY_PAGE_MAX;
            }
        }
        if (!valid) {
            reply.raw("{\"error\":\"invalid cursor or page limit\"}");
        } else {
            std::array<ayni_capability_info_v1, AYNI_CAPABILITY_PAGE_MAX> items{};
            uint32_t count = 0;
            rc = s->list_capabilities(s->call_context, &cursor, items.data(), limit, &count, &next);
            if (rc != AYNI_OK) {
                reply.raw(
                    "{\"error\":\"directory changed; restart with empty cursor\",\"status\":");
                reply.number(rc);
                reply.raw("}");
            } else {
                reply.raw("{\"capabilities\":[");
                for (uint32_t i = 0; i < count; ++i) {
                    if (i) {
                        reply.raw(",");
                    }
                    reply.raw("{\"binding_id\":");
                    reply.string(items[i].binding_id);
                    reply.raw(",\"binding_version\":");
                    reply.unsigned_number(items[i].binding_version);
                    reply.raw(",\"summary\":");
                    reply.string(items[i].summary);
                    reply.raw("}");
                }
                reply.raw("],\"next_cursor\":");
                write_cursor(reply, next);
                reply.raw("}");
            }
        }
    } else if (same(call.function_name, "ayni_inspect_capability") &&
               only(args, "|binding_id||binding_version|")) {
        const auto *id = field(args, 0, "binding_id"), *version = field(args, 0, "binding_version");
        auto detail = sized<ayni_capability_detail_v1>();
        if (id && version && id->kind == AYNI_ARGUMENT_STRING &&
            version->kind == AYNI_ARGUMENT_NUMBER && same(version->value.text, "1")) {
            rc = s->inspect_capability(s->call_context, id->value.text, 1, &detail);
        } else {
            rc = AYNI_INVALID_ARGUMENT;
        }
        if (rc == AYNI_OK) {
            reply.raw("{\"binding_id\":");
            reply.string(detail.info.binding_id);
            reply.raw(",\"binding_version\":1,\"purpose\":");
            reply.string(detail.usage.purpose);
            reply.raw(",\"input_schema\":");
            ayni::json::write_schema(reply, self.tool_schema(id->value.text));
            reply.raw(",\"text_usage\":");
            reply.string(detail.usage.text_usage);
            reply.raw(",\"result_usage\":");
            reply.string(detail.usage.result_usage);
            reply.raw("}");
        } else {
            reply.raw("{\"error\":\"not visible or invalid exact binding\"}");
        }
    } else if (same(call.function_name, "ayni_call_capability") &&
               only(args, "|operation||binding_id||binding_version||arguments|")) {
        const auto *id = field(args, 0, "binding_id"), *version = field(args, 0, "binding_version");
        const auto *input = field(args, 0, "arguments"), *operation = field(args, 0, "operation");
        const bool shape =
            input && id && id->kind == AYNI_ARGUMENT_STRING && version &&
            version->kind == AYNI_ARGUMENT_NUMBER && same(version->value.text, "1") &&
            (!operation ||
             (operation->kind == AYNI_ARGUMENT_STRING && same(operation->value.text, "invoke")));
        auto result = sized<ayni_task_call_result_v1>();
        auto detail = sized<ayni_capability_detail_v1>();
        auto schema = shape ? self.tool_schema(id->value.text) : ayni_schema_v1{};
        // Existing results keep their original meaning even if current directory
        // visibility changes while a later member of the same group is running.
        rc = s->call_result(s->call_context, step_key, &result);
        if (rc == AYNI_OK) {
            if (result.outcome != AYNI_CALL_COMPLETED || result.status != AYNI_OK) {
                char diagnostic[240];
                std::snprintf(diagnostic, sizeof(diagnostic),
                              "tool terminal binding=%.*s call=%llu outcome=%lu status=%ld "
                              "reason=%lu effect=%lu",
                              id && id->kind == AYNI_ARGUMENT_STRING ? int(id->value.text.size) : 0,
                              id && id->kind == AYNI_ARGUMENT_STRING ? id->value.text.data : "",
                              (unsigned long long)result.logical_call_id,
                              (unsigned long)result.outcome, (long)result.status,
                              (unsigned long)result.reason, (unsigned long)result.effect);
                self.status(self.turn_, diagnostic);
                intent_submitted = true;
                return report(*s, *out, AYNI_POLICY_REPORT_FAILURE);
            }
            reply.raw(std::string_view(result.text.data, result.text.size));
        } else if (rc != AYNI_NOT_FOUND) {
            return rc;
        } else if (!shape || !schema.nodes ||
                   s->inspect_capability(s->call_context, id->value.text, 1, &detail) != AYNI_OK) {
            reply.raw("{\"error\":\"invalid, unsupported or unauthorized tool binding\"}");
        } else {
            const auto root = uint32_t(input - args.nodes), count = input->subtree_end - root;
            if (count > self.input_arguments_.size()) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            for (uint32_t i = 0; i < count; ++i) {
                self.input_arguments_[i] = args.nodes[root + i];
                self.input_arguments_[i].subtree_end -= root;
            }
            self.input_arguments_[0].name = {};
            const ayni_arguments_v1 arguments{self.input_arguments_.data(), count};
            ayni_value_buffer_v1 buffer{
                self.input_values_.data(), uint32_t(self.input_values_.size()), 0,
                self.input_text_.data(),   uint32_t(self.input_text_.size()),   0};
            rc = ayni_arguments_normalize(&self.value_workspace_, &schema, &arguments, &buffer);
            if (rc == AYNI_OK) {
                auto request = sized<ayni_call_request_v1>();
                request.kind = AYNI_POLICY_REQUEST_CALL;
                request.binding_id = id->value.text;
                request.binding_version = 1;
                request.step_key = step_key;
                request.value = {buffer.nodes, buffer.node_count};
                intent_submitted = true;
                return out->request_call(out->call_context, &request);
            }
            reply.raw("{\"error\":\"arguments do not match the registered Schema\",\"status\":");
            reply.number(rc);
            reply.raw("}");
        }
    } else {
        reply.raw("{\"error\":\"unsupported function; shell is unavailable\"}");
    }
    size_t reply_bytes = 0;
    if (!reply.finish(reply_bytes)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    response = {self.tool_reply_.data(), uint32_t(reply_bytes)};
    return AYNI_OK;
}
} // namespace usbchat
