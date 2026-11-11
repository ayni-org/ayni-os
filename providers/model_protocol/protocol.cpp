#include "json.h"
#include <ayni/model_messages.hpp>
#include <ayni/providers/model_protocol.h>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <yyjson.h>

namespace ayni::providers::model_protocol {
namespace {
using ::ayni::json::Writer;
using detail::parse;
// Statistics describe the serving model, not a local approximation. A malformed
// optional usage object cannot erase an otherwise known model completion.
ayni_model_usage_v1 decode_usage(yyjson_val *root, Api api) {
    auto *usage = yyjson_obj_get(root, "usage");
    if (!usage || yyjson_is_null(usage)) {
        return {};
    }
    const ayni_model_usage_v1 invalid{AYNI_MODEL_USAGE_INVALID, 0, 0, 0, 0};
    auto *input = yyjson_obj_get(usage, api == Api::responses ? "input_tokens" : "prompt_tokens");
    auto *output =
        yyjson_obj_get(usage, api == Api::responses ? "output_tokens" : "completion_tokens");
    auto *total = yyjson_obj_get(usage, "total_tokens");
    if (!yyjson_is_uint(input) || !yyjson_is_uint(output) || !yyjson_is_uint(total)) {
        return invalid;
    }
    const auto in = yyjson_get_uint(input);
    const auto out = yyjson_get_uint(output);
    const auto sum = yyjson_get_uint(total);
    if (in > UINT64_MAX - out || in + out != sum) {
        return invalid;
    }
    return {AYNI_MODEL_USAGE_REPORTED, 0, in, out, sum};
}
bool copy_body(yyjson_val *value, Answer &answer, ayni_string_v1 &out) {
    if (!yyjson_is_str(value) || yyjson_get_len(value) > answer.body.size() - answer.used ||
        yyjson_get_len(value) > UINT32_MAX) {
        return false;
    }
    const size_t size = yyjson_get_len(value);
    std::memcpy(answer.body.data() + answer.used, yyjson_get_str(value), size);
    out = {answer.body.data() + answer.used, static_cast<uint32_t>(size)};
    answer.used += size;
    return true;
}
// Copies every name/string/number lexeme before the arguments DOM is released.
// The owner maps these nodes against its original Schema, never double-derived integers.
ayni_status_t decode_arguments(yyjson_val *value, ayni_string_v1 name, Answer &answer,
                               std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> &nodes,
                               uint32_t &count) {
    if (count == nodes.size()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const uint32_t index = count++;
    auto &node = nodes[index];
    node = {};
    const auto save = [&](const char *data, size_t size, ayni_string_v1 &out) {
        if (size > answer.body.size() - answer.used || size > UINT32_MAX) {
            return false;
        }
        if (size) {
            std::memcpy(answer.body.data() + answer.used, data, size);
        }
        out = {answer.body.data() + answer.used, static_cast<uint32_t>(size)};
        answer.used += size;
        return true;
    };
    if (!save(name.data, name.size, node.name)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (yyjson_is_obj(value)) {
        node.kind = AYNI_ARGUMENT_OBJECT;
        size_t i, size;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, i, size, key, child) {
            const auto status = decode_arguments(
                child, {yyjson_get_str(key), static_cast<uint32_t>(yyjson_get_len(key))}, answer,
                nodes, count);
            if (status != AYNI_OK) {
                return status;
            }
        }
    } else if (yyjson_is_arr(value)) {
        node.kind = AYNI_ARGUMENT_LIST;
        size_t i, size;
        yyjson_val *child;
        yyjson_arr_foreach(value, i, size, child) {
            const auto status = decode_arguments(child, {}, answer, nodes, count);
            if (status != AYNI_OK) {
                return status;
            }
        }
    } else if (yyjson_is_bool(value)) {
        node.kind = AYNI_ARGUMENT_BOOL;
        node.value.boolean = yyjson_get_bool(value) ? 1U : 0U;
    } else if (yyjson_is_null(value)) {
        node.kind = AYNI_ARGUMENT_NULL;
    } else if (yyjson_is_str(value) || yyjson_is_raw(value)) {
        node.kind = yyjson_is_str(value) ? AYNI_ARGUMENT_STRING : AYNI_ARGUMENT_NUMBER;
        if (!save(yyjson_is_str(value) ? yyjson_get_str(value) : yyjson_get_raw(value),
                  yyjson_get_len(value), node.value.text)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    } else {
        return AYNI_INVALID_ARGUMENT;
    }
    node.subtree_end = count;
    return AYNI_OK;
}
ayni_status_t decode_tool(yyjson_val *call, size_t at, Answer &answer, char *pool, size_t pool_size,
                          Api api) {
    if (!yyjson_equals_str(yyjson_obj_get(call, "type"),
                           api == Api::responses ? "function_call" : "function")) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto *function = api == Api::responses ? call : yyjson_obj_get(call, "function");
    auto *args = yyjson_obj_get(function, "arguments");
    auto *status = yyjson_obj_get(call, "status");
    if (api == Api::responses && status && !yyjson_equals_str(status, "completed")) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &decoded = answer.calls[at];
    decoded = {};
    decoded.struct_size = sizeof(decoded);
    if (!copy_body(yyjson_obj_get(call, api == Api::responses ? "call_id" : "id"), answer,
                   decoded.protocol_id) ||
        decoded.protocol_id.size == 0 || decoded.protocol_id.size > AYNI_MODEL_TOOL_ID_BYTES_MAX ||
        !copy_body(yyjson_obj_get(function, "name"), answer, decoded.function_name) ||
        decoded.function_name.size == 0 || decoded.function_name.size > AYNI_CATALOG_ID_MAX_BYTES ||
        !yyjson_is_str(args)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto source_begin = answer.used;
    ayni_status_t parsed = AYNI_OK;
    auto arguments = parse(const_cast<char *>(yyjson_get_str(args)), yyjson_get_len(args), pool,
                           pool_size, parsed, true);
    uint32_t count = 0;
    if (arguments) {
        parsed = decode_arguments(yyjson_doc_get_root(arguments.get()), {}, answer,
                                  answer.arguments[at], count);
        if (parsed == AYNI_OK) {
            decoded.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
            decoded.input.arguments = {answer.arguments[at].data(), count};
            parsed = ayni_arguments_validate(&decoded.input.arguments);
        }
    }
    if (parsed != AYNI_OK) {
        // Keep source identity and complete raw arguments for a matching tool error.
        // Reclaim a partial tree's string lane; no second execution representation survives.
        answer.used = source_begin;
        decoded.input_kind = AYNI_TOOL_INPUT_INVALID_ARGUMENTS;
        if (!copy_body(args, answer, decoded.input.invalid_arguments)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    return AYNI_OK;
}
} // namespace
Buffer::~Buffer() {
    reset();
}
void Buffer::reset() {
    auto *bytes = reinterpret_cast<volatile unsigned char *>(data_);
    for (size_t at = 0; at < size_; ++at) {
        bytes[at] = 0;
    }
    std::free(data_);
    data_ = nullptr;
    size_ = 0;
}
bool Buffer::resize(size_t size) {
    if (size == size_) {
        return true;
    }
    auto *next = size ? static_cast<char *>(std::malloc(size)) : nullptr;
    if (size && !next) {
        return false;
    }
    reset();
    data_ = next;
    size_ = size;
    return true;
}

Answer::~Answer() {
    clear();
}
void Answer::clear() {
    body.reset();
    for (auto *value : {&continuation, &reply}) {
        volatile char *bytes = value->data();
        for (size_t at = 0; at < value->size(); ++at) {
            bytes[at] = 0;
        }
        std::string{}.swap(*value);
    }
    used = 0;
    result = {};
}
void Answer::fail(ayni_status_t status, ayni_reason_t reason) {
    result.outcome = status == AYNI_RESULT_UNKNOWN ? AYNI_CALL_ABANDONED : AYNI_CALL_FAILED;
    result.status = status;
    result.reason = status == AYNI_RESULT_UNKNOWN ? AYNI_REASON_NONE : reason;
    // Invalid model output does not establish whether egress/billing occurred.
    result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    result.result = {};
    result.tool_call_count = 0;
    result.tool_calls = nullptr;
    result.body = {};
}
void Protocol::decode(char *buffer, size_t size, Answer &answer) const {
    decode(RequestOptions{}, buffer, size, answer);
}
void Protocol::decode(const RequestOptions &options, char *buffer, size_t size,
                      Answer &answer) const {
    // Every decoded string is a piece of this response, copied at most twice (a tool's raw
    // arguments and their parsed strings), so the body never grows during decode.
    const auto attempt = answer.result.attempt;
    answer.clear();
    answer.result.struct_size = sizeof(answer.result);
    answer.result.attempt = attempt;
    answer.fail(AYNI_INVALID_ARGUMENT, AYNI_REASON_MODEL_OUTPUT);
    if (size > UINT32_MAX / 2U) {
        answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    const auto pool_size = yyjson_read_max_memory_usage(size, 0);
    // DOMs are destroyed before these private pools; every allocation failure is explicit.
    Buffer outer_pool, arguments_pool;
    if (!outer_pool.resize(pool_size) || !arguments_pool.resize(pool_size) ||
        !answer.body.resize(2 * size)) {
        answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    ayni_status_t parsed = AYNI_OK;
    bool invalid_usage = false;
    auto doc = parse(buffer, size, outer_pool.data(), pool_size, parsed, false, &invalid_usage,
                     envelope_depth_max, envelope_array_max);
    if (!doc) {
        answer.fail(parsed, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    auto *root = yyjson_doc_get_root(doc.get());
    answer.result.model_usage = invalid_usage
                                    ? ayni_model_usage_v1{AYNI_MODEL_USAGE_INVALID, 0, 0, 0, 0}
                                    : decode_usage(root, options.api);
    if (options.api == Api::responses) {
        if (yyjson_equals_str(yyjson_obj_get(root, "status"), "failed") ||
            yyjson_equals_str(yyjson_obj_get(root, "status"), "incomplete")) {
            answer.fail(AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
            return;
        }
        if (options.reply_encoding != model::ReplyEncoding::framed ||
            !yyjson_equals_str(yyjson_obj_get(root, "status"), "completed") ||
            !yyjson_is_arr(yyjson_obj_get(root, "output"))) {
            return;
        }
        auto *items = yyjson_obj_get(root, "output");
        uint32_t count = 0;
        std::string text;
        size_t at, item_count;
        yyjson_val *item;
        yyjson_arr_foreach(items, at, item_count, item) {
            auto *type = yyjson_obj_get(item, "type");
            if (yyjson_equals_str(type, "function_call")) {
                if (count == AYNI_CALL_TOOL_COUNT_MAX) {
                    answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
                    return;
                }
                const auto status = decode_tool(item, count, answer, arguments_pool.data(),
                                                pool_size, Api::responses);
                if (status != AYNI_OK) {
                    answer.fail(status, AYNI_REASON_MODEL_OUTPUT);
                    return;
                }
                for (uint32_t before = 0; before < count; ++before) {
                    if (std::string_view(answer.calls[before].protocol_id.data,
                                         answer.calls[before].protocol_id.size) ==
                        std::string_view(answer.calls[count].protocol_id.data,
                                         answer.calls[count].protocol_id.size)) {
                        return;
                    }
                }
                ++count;
            } else if (yyjson_equals_str(type, "message")) {
                if (!detail::append_response_text(item, text)) {
                    return;
                }
            } else if (!detail::valid_reasoning(item)) {
                return; // Unsupported output must not be silently discarded or reinterpreted.
            }
        }
        if (text.size() > answer.body.size() - answer.used || (!count && text.empty())) {
            return;
        }
        std::memcpy(answer.body.data() + answer.used, text.data(), text.size());
        answer.result.result = {answer.body.data() + answer.used,
                                static_cast<uint32_t>(text.size())};
        answer.used += text.size();
        answer.result.tool_call_count = count;
        answer.result.tool_calls = count ? answer.calls.data() : nullptr;
        const auto write = [&](Writer &out) {
            out.raw("{\"model\":");
            out.string(options.model);
            out.raw(",\"scope\":");
            out.string(options.continuation_scope);
            out.raw(",\"output\":");
            detail::write_json(out, items);
            out.raw("}");
        };
        auto measure = Writer::measuring();
        write(measure);
        size_t bytes = 0;
        if (!measure.finish(bytes)) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        answer.continuation.resize(bytes);
        Writer out(answer.continuation.data(), bytes);
        write(out);
        if (!out.finish(bytes)) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        answer.result.outcome = AYNI_CALL_COMPLETED;
        answer.result.status = AYNI_OK;
        answer.result.reason = AYNI_REASON_NONE;
        answer.result.effect = AYNI_CALL_EFFECT_COMPLETE;
        return;
    }
    // Only an explicitly finished singleton choice is business evidence. Stream accumulation
    // reaches this same validation only after its terminal marker and successful HTTP EOF.
    auto *choices = yyjson_obj_get(root, "choices");
    if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) != 1) {
        return;
    }
    auto *choice = yyjson_arr_get_first(choices);
    auto *finish = yyjson_obj_get(choice, "finish_reason");
    auto *message = yyjson_obj_get(choice, "message");
    if (!yyjson_equals_str(yyjson_obj_get(message, "role"), "assistant")) {
        return;
    }
    auto *calls = yyjson_obj_get(message, "tool_calls");
    auto *content = yyjson_obj_get(message, "content");
    const auto count = yyjson_arr_size(calls);
    if (calls && !yyjson_is_null(calls) && !yyjson_is_arr(calls)) {
        return;
    }
    if (count == 0) {
        if (!yyjson_equals_str(finish, "stop") ||
            !copy_body(content, answer, answer.result.result)) {
            return;
        }
    } else {
        if (!yyjson_equals_str(finish, "tool_calls")) {
            return;
        }
        if (content && !yyjson_is_null(content) &&
            !copy_body(content, answer, answer.result.result)) {
            return;
        }
        if (count > AYNI_CALL_TOOL_COUNT_MAX) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        for (size_t at = 0; at < count; ++at) {
            const auto status =
                decode_tool(yyjson_arr_get(calls, at), at, answer, arguments_pool.data(), pool_size,
                            Api::chat_completions);
            if (status != AYNI_OK) {
                answer.fail(status, AYNI_REASON_MODEL_OUTPUT);
                return;
            }
        }
        answer.result.tool_call_count = static_cast<uint32_t>(count);
        answer.result.tool_calls = answer.calls.data();
    }
    if (options.reply_encoding == model::ReplyEncoding::framed) {
        const auto write = [&](Writer &out) {
            out.raw("{\"model\":");
            out.string(options.model);
            out.raw(",\"scope\":");
            out.string(options.continuation_scope);
            out.raw("}");
        };
        auto measured = Writer::measuring();
        write(measured);
        size_t bytes = 0;
        if (!measured.finish(bytes)) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        answer.continuation.resize(bytes);
        Writer out(answer.continuation.data(), bytes);
        write(out);
        if (!out.finish(bytes)) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
    }
    answer.result.outcome = AYNI_CALL_COMPLETED;
    answer.result.status = AYNI_OK;
    answer.result.reason = AYNI_REASON_NONE;
    answer.result.effect = AYNI_CALL_EFFECT_COMPLETE;
}
} // namespace ayni::providers::model_protocol
