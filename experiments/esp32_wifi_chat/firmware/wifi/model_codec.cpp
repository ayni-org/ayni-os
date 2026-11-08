#include "model_codec.h"
#include "../main/model_tools.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr size_t response_bytes = 32768, parse_bytes = 512 * 1024,
                 argument_parse_bytes = 128 * 1024;
using Memory = std::unique_ptr<void, decltype(&std::free)>;
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
bool shape(yyjson_val *value, size_t depth = 0) {
    if (depth > 12) {
        return false;
    }
    if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) > 64) {
            return false;
        }
        size_t at, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, at, count, key, child) {
            if (yyjson_obj_getn(value, yyjson_get_str(key), yyjson_get_len(key)) != child ||
                !shape(child, depth + 1)) {
                return false;
            }
        }
    } else if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > 64) {
            return false;
        }
        size_t at, count;
        yyjson_val *child;
        yyjson_arr_foreach(value, at, count, child) {
            if (!shape(child, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}
ayni_string_v1 text(yyjson_val *value) {
    return yyjson_is_str(value)
               ? ayni_string_v1{yyjson_get_str(value), uint32_t(yyjson_get_len(value))}
               : ayni_string_v1{};
}
bool arguments(yyjson_val *value, ayni_string_v1 name, ayni_argument_node_v1 *nodes,
               uint32_t &count, size_t depth = 0) {
    if (depth > 8 || count >= ModelTools::nodes_per_tool) {
        return false;
    }
    const auto index = count++;
    auto &node = nodes[index];
    node = {};
    node.name = name;
    if (yyjson_is_obj(value)) {
        node.kind = AYNI_ARGUMENT_OBJECT;
        size_t at, size;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, at, size, key, child) {
            if (!arguments(child, text(key), nodes, count, depth + 1)) {
                return false;
            }
        }
    } else if (yyjson_is_arr(value)) {
        node.kind = AYNI_ARGUMENT_LIST;
        size_t at, size;
        yyjson_val *child;
        yyjson_arr_foreach(value, at, size, child) {
            if (!arguments(child, {}, nodes, count, depth + 1)) {
                return false;
            }
        }
    } else if (yyjson_is_str(value)) {
        node.kind = AYNI_ARGUMENT_STRING;
        node.value.text = text(value);
    } else if (yyjson_is_raw(value)) {
        node.kind = AYNI_ARGUMENT_NUMBER;
        node.value.text = {yyjson_get_raw(value), uint32_t(yyjson_get_len(value))};
    } else if (yyjson_is_bool(value)) {
        node.kind = AYNI_ARGUMENT_BOOL;
        node.value.boolean = yyjson_get_bool(value);
    } else if (yyjson_is_null(value)) {
        node.kind = AYNI_ARGUMENT_NULL;
    } else {
        return false;
    }
    node.subtree_end = count;
    return true;
}
struct ArgumentDocument {
    void *memory = nullptr;
    yyjson_doc *doc = nullptr;
    ~ArgumentDocument() {
        if (doc) {
            yyjson_doc_free(doc);
        }
        std::free(memory);
    }
};
struct Proposals {
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    std::array<std::array<ayni_argument_node_v1, ModelTools::nodes_per_tool>,
               AYNI_CALL_TOOL_COUNT_MAX>
        nodes{};
    std::array<ArgumentDocument, AYNI_CALL_TOOL_COUNT_MAX> documents{};
};
} // namespace
bool decode_model(std::string_view response, bool tools_allowed, Packet &result) {
    if (response.empty() || response.size() > response_bytes) {
        return false;
    }
    const auto pool_bytes = std::min(
        parse_bytes, yyjson_read_max_memory_usage(response.size(), YYJSON_READ_NUMBER_AS_RAW));
    Memory pool(std::malloc(pool_bytes), std::free);
    yyjson_alc allocator{};
    if (!pool || !yyjson_alc_pool_init(&allocator, pool.get(), pool_bytes)) {
        return false;
    }
    Document doc(yyjson_read_opts(const_cast<char *>(response.data()), response.size(),
                                  YYJSON_READ_NUMBER_AS_RAW, &allocator, nullptr),
                 yyjson_doc_free);
    if (!doc || !shape(yyjson_doc_get_root(doc.get()))) {
        return false;
    }
    auto *choices = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "choices");
    if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) != 1) {
        return false;
    }
    auto *choice = yyjson_arr_get_first(choices), *message = yyjson_obj_get(choice, "message");
    if (!yyjson_equals_str(yyjson_obj_get(message, "role"), "assistant")) {
        return false;
    }
    auto *calls = yyjson_obj_get(message, "tool_calls"),
         *finish = yyjson_obj_get(choice, "finish_reason");
    if (calls && !yyjson_is_null(calls) && !yyjson_is_arr(calls)) {
        return false;
    }
    const auto count = yyjson_arr_size(calls);
    const auto body = text(yyjson_obj_get(message, "content"));
    if (!count) {
        if (!yyjson_equals_str(finish, "stop") || !body.size || body.size > summary_bytes) {
            return false;
        }
        result.kind = Kind::model_text;
        result.size = body.size;
        std::memcpy(result.body.data(), body.data, body.size);
        return true;
    }
    if (!tools_allowed || count > AYNI_CALL_TOOL_COUNT_MAX ||
        !yyjson_equals_str(finish, "tool_calls") || body.size > answer_bytes) {
        return false;
    }
    auto proposals = std::unique_ptr<Proposals>(new (std::nothrow) Proposals{});
    if (!proposals) {
        return false;
    }
    for (size_t at = 0; at < count; ++at) {
        auto *item = yyjson_arr_get(calls, at), *function = yyjson_obj_get(item, "function");
        const auto raw = text(yyjson_obj_get(function, "arguments"));
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function") || !raw.data ||
            raw.size > ModelTools::wire_bytes) {
            return false;
        }
        auto &call = proposals->calls[at];
        call.struct_size = sizeof(call);
        call.protocol_id = text(yyjson_obj_get(item, "id"));
        call.function_name = text(yyjson_obj_get(function, "name"));
        auto &argument = proposals->documents[at];
        const auto capacity =
            std::min(argument_parse_bytes,
                     yyjson_read_max_memory_usage(raw.size, YYJSON_READ_NUMBER_AS_RAW));
        argument.memory = std::malloc(capacity);
        yyjson_alc arg_allocator{};
        if (!argument.memory || !yyjson_alc_pool_init(&arg_allocator, argument.memory, capacity)) {
            return false;
        }
        argument.doc = yyjson_read_opts(const_cast<char *>(raw.data), raw.size,
                                        YYJSON_READ_NUMBER_AS_RAW, &arg_allocator, nullptr);
        if (!argument.doc) {
            call.input_kind = AYNI_TOOL_INPUT_INVALID_ARGUMENTS;
            call.input.invalid_arguments = raw;
        } else {
            uint32_t nodes = 0;
            if (!arguments(yyjson_doc_get_root(argument.doc), {}, proposals->nodes[at].data(),
                           nodes)) {
                return false;
            }
            call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
            call.input.arguments = {proposals->nodes[at].data(), nodes};
        }
    }
    uint32_t written = 0;
    if (ayni_tool_calls_write(proposals->calls.data(), count, body, result.body.data(),
                              ModelTools::wire_bytes, &written) != AYNI_OK) {
        return false;
    }
    result.kind = Kind::model_tools;
    result.size = written;
    return true;
}
} // namespace wifichat::wifi
