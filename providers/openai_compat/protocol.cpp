#include "protocol.h"
#include <ayni/model_messages.hpp>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <yyjson.h>

namespace ayni::providers::openai_compat {
namespace {
using ::ayni::json::Writer;
// JSON DOM strings can include private model input/results. Declare this guard
// before any Document so all DOM destructors run before either pool is cleared.
struct EraseScratch {
    char *first;
    char *second;
    size_t bytes;
    ~EraseScratch() {
        for (char *pool : {first, second}) {
            auto *current = reinterpret_cast<volatile unsigned char *>(pool);
            for (size_t at = 0; at < bytes; ++at) {
                current[at] = 0;
            }
        }
    }
};

// Check depth before the parser creates a DOM. yyjson remains the syntax/UTF-8
// validator; braces inside escaped strings cannot increase the structural depth.
bool depth_valid(const char *data, size_t size) {
    uint32_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > json_depth_max) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return depth == 0 && !quoted;
}

enum class ShapeRegion { body, response, usage };
ayni_status_t validate_shape(yyjson_val *value, ShapeRegion region, bool &invalid_usage) {
    if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > json_container_max) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *child;
        yyjson_arr_foreach(value, i, count, child) {
            const auto status = validate_shape(
                child, region == ShapeRegion::usage ? ShapeRegion::usage : ShapeRegion::body,
                invalid_usage);
            if (status != AYNI_OK) {
                return status;
            }
        }
    } else if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) > AYNI_SCHEMA_NODE_COUNT_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, i, count, key, child) {
            const auto child_region =
                region == ShapeRegion::usage ||
                        (region == ShapeRegion::response && yyjson_equals_str(key, "usage"))
                    ? ShapeRegion::usage
                    : ShapeRegion::body;
            // Ambiguity in optional usage is local to accounting. Response and
            // argument identities remain strict; capacity checks still walk all data.
            if (yyjson_obj_getn(value, yyjson_get_str(key), yyjson_get_len(key)) != child) {
                if (child_region != ShapeRegion::usage) {
                    return AYNI_INVALID_ARGUMENT;
                }
                invalid_usage = true;
            }
            const auto status = validate_shape(child, child_region, invalid_usage);
            if (status != AYNI_OK) {
                return status;
            }
        }
    }
    return AYNI_OK;
}
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
// Statistics describe the serving model, not a local approximation. A malformed
// optional usage object cannot erase an otherwise known model completion.
ayni_model_usage_v1 decode_usage(yyjson_val *root) {
    auto *usage = yyjson_obj_get(root, "usage");
    if (!usage || yyjson_is_null(usage)) {
        return {};
    }
    const ayni_model_usage_v1 invalid{AYNI_MODEL_USAGE_INVALID, 0, 0, 0, 0};
    auto *input = yyjson_obj_get(usage, "prompt_tokens");
    auto *output = yyjson_obj_get(usage, "completion_tokens");
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
Document parse(char *data, size_t size, char *pool, size_t pool_size, ayni_status_t &status,
               bool raw_numbers = false, bool *invalid_usage = nullptr) {
    status = AYNI_INVALID_ARGUMENT;
    yyjson_alc allocator{};
    if (!depth_valid(data, size) || !yyjson_alc_pool_init(&allocator, pool, pool_size)) {
        return {nullptr, yyjson_doc_free};
    }
    Document doc(yyjson_read_opts(data, size, raw_numbers ? YYJSON_READ_NUMBER_AS_RAW : 0,
                                  &allocator, nullptr),
                 yyjson_doc_free);
    if (doc) {
        bool ignored = false;
        status = validate_shape(yyjson_doc_get_root(doc.get()),
                                invalid_usage ? ShapeRegion::response : ShapeRegion::body,
                                invalid_usage ? *invalid_usage : ignored);
        if (status != AYNI_OK) {
            doc.reset();
        }
    }
    return doc;
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
ayni_status_t decode_tool(yyjson_val *call, size_t at, Answer &answer, char *pool,
                          size_t pool_size) {
    if (!yyjson_equals_str(yyjson_obj_get(call, "type"), "function")) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto *function = yyjson_obj_get(call, "function");
    auto *args = yyjson_obj_get(function, "arguments");
    auto &decoded = answer.calls[at];
    decoded = {};
    decoded.struct_size = sizeof(decoded);
    if (!copy_body(yyjson_obj_get(call, "id"), answer, decoded.protocol_id) ||
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

void Answer::fail(ayni_status_t status, ayni_reason_t reason) {
    result.outcome = AYNI_CALL_FAILED;
    result.status = status;
    result.reason = reason;
    // Invalid model output does not establish whether egress/billing occurred.
    result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    result.result = {};
    result.tool_call_count = 0;
    result.tool_calls = nullptr;
    result.body = {};
}
namespace {
ayni_status_t write_request(const Config &config, ayni_string_v1 input, Writer &out) {
    out.raw("{\"model\":");
    out.string(config.model);
    out.raw(",\"max_tokens\":");
    out.number(config.max_output_tokens);
    out.raw(",\"stream\":false,");
    if (std::string_view(input.data, input.size).substr(0, model::messages_prefix.size()) ==
        model::messages_prefix) {
        const auto status = model::write_context(out, input,
                                                 config.result_format == AYNI_CALL_RESULT_TOOL_CALLS
                                                     ? model::Mode::assistant
                                                     : model::Mode::text);
        if (status != AYNI_OK) {
            return status;
        }
    } else {
        ayni_string_v1 instructions{}, data{};
        const auto status = model::split_instructions(input, instructions, data);
        if (status != AYNI_OK) {
            return status;
        }
        out.raw("\"messages\":[");
        if (instructions.size) {
            out.raw("{\"role\":\"system\",\"content\":");
            out.string(instructions);
            out.raw("},");
        }
        out.raw("{\"role\":\"user\",\"content\":");
        out.string(data);
        out.raw("}]");
        if (config.result_format == AYNI_CALL_RESULT_TOOL_CALLS) {
            out.raw(",\"tools\":");
            model::write_functions(out);
        }
    }
    out.raw("}");
    return AYNI_OK;
}
} // namespace
ayni_status_t Protocol::encode(const Config &config, ayni_string_v1 input,
                               std::string &request) const {
    auto measure = Writer::measuring();
    auto status = write_request(config, input, measure);
    size_t size = 0;
    if (status != AYNI_OK || !measure.finish(size)) {
        return status != AYNI_OK ? status : AYNI_CAPACITY_EXCEEDED;
    }
    request.assign(size, '\0');
    Writer out(request.data(), request.size());
    status = write_request(config, input, out);
    size_t written = 0;
    return status != AYNI_OK                        ? status
           : out.finish(written) && written == size ? AYNI_OK
                                                    : AYNI_INTERNAL;
}

// Only an explicitly finished singleton choice is business evidence. Protocol
// metadata can be ignored after bounded validation, but arguments must be exact.
void Protocol::decode(char *buffer, size_t size, Answer &answer) const {
    // Every decoded string is a piece of this response, copied at most twice (a tool's raw
    // arguments and their parsed strings), so the body never grows during decode.
    const auto pool_size = yyjson_read_max_memory_usage(size, 0);
    std::unique_ptr<char[]> outer_pool(new char[pool_size]), arguments_pool(new char[pool_size]);
    EraseScratch erase{outer_pool.get(), arguments_pool.get(), pool_size};
    answer.body.assign(2 * size, '\0');
    answer.used = 0;
    answer.result.model_usage = {};
    answer.fail(AYNI_INVALID_ARGUMENT, AYNI_REASON_MODEL_OUTPUT);
    ayni_status_t parsed = AYNI_OK;
    bool invalid_usage = false;
    auto doc = parse(buffer, size, outer_pool.get(), pool_size, parsed, false, &invalid_usage);
    if (!doc) {
        answer.fail(parsed, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    auto *root = yyjson_doc_get_root(doc.get());
    answer.result.model_usage = invalid_usage
                                    ? ayni_model_usage_v1{AYNI_MODEL_USAGE_INVALID, 0, 0, 0, 0}
                                    : decode_usage(root);
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
                decode_tool(yyjson_arr_get(calls, at), at, answer, arguments_pool.get(), pool_size);
            if (status != AYNI_OK) {
                answer.fail(status, AYNI_REASON_MODEL_OUTPUT);
                return;
            }
        }
        answer.result.tool_call_count = static_cast<uint32_t>(count);
        answer.result.tool_calls = answer.calls.data();
    }
    answer.result.outcome = AYNI_CALL_COMPLETED;
    answer.result.status = AYNI_OK;
    answer.result.reason = AYNI_REASON_NONE;
    answer.result.effect = AYNI_CALL_EFFECT_COMPLETE;
}
} // namespace ayni::providers::openai_compat
