#include "tool_plan.h"
#include <algorithm>
#include <array>
#include <ayni/model_messages.hpp>
#include <cstdio>
#include <cstring>
#include <new>
#include <yyjson.h>
namespace ayni::providers::mock {
namespace {
void erase(char *bytes, size_t size) {
    auto *out = reinterpret_cast<volatile unsigned char *>(bytes);
    while (size--) {
        *out++ = 0;
    }
}
constexpr std::string_view call_prefix = "call:";
constexpr std::string_view function_prefix = "function:";
constexpr std::string_view json_prefix = "json:";
} // namespace
struct ToolPlanner::Impl {
    struct Plan {
        ayni_status_t status = AYNI_OK;
        uint32_t count = 0;
        uint32_t node_count = 0, used = 0;
        ayni_string_v1 body{};
        ayni_attempt_ref_v1 attempt{};
        std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
        std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
        std::array<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>,
                   AYNI_CALL_TOOL_COUNT_MAX>
            wrapped{};
        std::array<ayni_string_v1, AYNI_CALL_TOOL_COUNT_MAX> names{};
        char ids[AYNI_CALL_TOOL_COUNT_MAX][AYNI_MODEL_TOOL_ID_BYTES_MAX]{};
    };
    uint32_t text_limit = 0, result_limit = 0;
    size_t pool_size = 0;
    std::unique_ptr<Plan[]> plans;
    std::unique_ptr<char[]> json_bytes, pool;
    // yyjson handles syntax only. Numeric lexemes survive unchanged; Core alone
    // matches names, defaults, numeric representation and authority to original Schema.
    ayni_status_t source(yyjson_val *value, ayni_string_v1 name, Plan &out, char *bytes,
                         uint32_t depth) {
        if (depth > AYNI_VALUE_DEPTH_MAX || out.node_count == out.nodes.size() ||
            name.size > AYNI_SCHEMA_NAME_BYTES_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const uint32_t at = out.node_count++;
        auto &node = out.nodes[at];
        const auto save = [&](const char *data, size_t size, ayni_string_v1 &target) {
            if (size > text_limit - out.used) {
                return false;
            }
            if (size) {
                std::memcpy(bytes + out.used, data, size);
            }
            target = {bytes + out.used, static_cast<uint32_t>(size)};
            out.used += static_cast<uint32_t>(size);
            return true;
        };
        if (!save(name.data, name.size, node.name)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        if (yyjson_is_obj(value) || yyjson_is_arr(value)) {
            const bool object = yyjson_is_obj(value);
            node.kind = object ? AYNI_ARGUMENT_OBJECT : AYNI_ARGUMENT_LIST;
            if ((object ? yyjson_obj_size(value) : yyjson_arr_size(value)) >
                (object ? AYNI_SCHEMA_NODE_COUNT_MAX : AYNI_VALUE_LIST_COUNT_MAX)) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            if (object) {
                size_t index, size;
                yyjson_val *key, *child;
                yyjson_obj_foreach(value, index, size, key, child) {
                    const auto status = source(
                        child, {yyjson_get_str(key), static_cast<uint32_t>(yyjson_get_len(key))},
                        out, bytes, depth + 1U);
                    if (status != AYNI_OK) {
                        return status;
                    }
                }
            } else {
                size_t index, size;
                yyjson_val *child;
                yyjson_arr_foreach(value, index, size, child) {
                    const auto status = source(child, {}, out, bytes, depth + 1U);
                    if (status != AYNI_OK) {
                        return status;
                    }
                }
            }
        } else if (yyjson_is_null(value)) {
            node.kind = AYNI_ARGUMENT_NULL;
        } else if (yyjson_is_bool(value)) {
            node.kind = AYNI_ARGUMENT_BOOL;
            node.value.boolean = yyjson_get_bool(value) ? 1U : 0U;
        } else if (yyjson_is_str(value) || yyjson_is_raw(value)) {
            node.kind = yyjson_is_str(value) ? AYNI_ARGUMENT_STRING : AYNI_ARGUMENT_NUMBER;
            if (!save(yyjson_is_str(value) ? yyjson_get_str(value) : yyjson_get_raw(value),
                      yyjson_get_len(value), node.value.text)) {
                return AYNI_CAPACITY_EXCEEDED;
            }
        } else {
            return AYNI_INVALID_ARGUMENT;
        }
        node.subtree_end = out.node_count;
        return AYNI_OK;
    }
    ayni_status_t parse_json(uint32_t index, const char *data, uint32_t size) {
        yyjson_alc allocator{};
        if (!yyjson_alc_pool_init(&allocator, pool.get(), pool_size)) {
            return AYNI_INTERNAL;
        }
        yyjson_read_err error{};
        auto *document = yyjson_read_opts(const_cast<char *>(data), size, YYJSON_READ_NUMBER_AS_RAW,
                                          &allocator, &error);
        auto &out = plans[index];
        const auto status = document ? source(yyjson_doc_get_root(document), {}, out,
                                              json_bytes.get() + index * text_limit, 1)
                                     : AYNI_INVALID_ARGUMENT;
        if (document) {
            yyjson_doc_free(document);
        }
        erase(pool.get(), pool_size);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_arguments_v1 arguments{out.nodes.data(), out.node_count};
        return ayni_arguments_validate(&arguments);
    }
    // Fixture syntax names native functions. call:<binding> is deterministic
    // sugar for the generic function with exact version 1, never catalog lookup.
    void plan(uint32_t index, const ayni_provider_request_v1 &request, ayni_string_v1 input) {
        auto &out = plans[index];
        out.attempt = request.attempt;
        std::string_view body(input.data, input.size);
        if (body.substr(0, model::messages_prefix.size()) == model::messages_prefix) {
            model::Reader reader(input);
            model::Message message;
            ayni_string_v1 selected{};
            while (reader.next(message)) {
                if (message.role == model::Role::user) {
                    selected = {};
                    const std::string_view text(message.text.data, message.text.size);
                    if (text.substr(0, call_prefix.size()) == call_prefix ||
                        text.substr(0, function_prefix.size()) == function_prefix) {
                        selected = message.text;
                    }
                } else if (message.role == model::Role::tool ||
                           message.role == model::Role::assistant) {
                    selected = {};
                }
            }
            if (!reader.finished()) {
                out.status = AYNI_INVALID_ARGUMENT;
                return;
            }
            body = {selected.data, selected.size};
        }
        const bool generic = body.substr(0, call_prefix.size()) == call_prefix;
        const auto prefix = generic ? call_prefix : function_prefix;
        while (body.substr(0, prefix.size()) == prefix) {
            if (out.count == out.names.size()) {
                out.status = AYNI_CAPACITY_EXCEEDED;
                return;
            }
            body.remove_prefix(prefix.size());
            const auto end = body.find(' ');
            const auto name = body.substr(0, end);
            if (name.empty() || name.size() > AYNI_CATALOG_ID_MAX_BYTES) {
                out.status = AYNI_INVALID_ARGUMENT;
                return;
            }
            out.names[out.count++] = {name.data(), static_cast<uint32_t>(name.size())};
            body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1U);
        }
        if (!out.count) {
            return;
        }
        const bool json = body.substr(0, json_prefix.size()) == json_prefix;
        if (json) {
            body.remove_prefix(json_prefix.size());
            out.status = parse_json(index, body.data(), static_cast<uint32_t>(body.size()));
        } else {
            out.node_count = 1;
            out.nodes[0].kind = AYNI_ARGUMENT_STRING;
            out.nodes[0].subtree_end = 1;
            out.nodes[0].value.text = {body.data(), static_cast<uint32_t>(body.size())};
        }
        if (out.status != AYNI_OK) {
            return;
        }
        for (uint32_t at = 0; at < out.count; ++at) {
            auto &call = out.calls[at];
            call.struct_size = sizeof(call);
            call.function_name =
                generic ? ayni_string_v1{model::call_function.data(),
                                         static_cast<uint32_t>(model::call_function.size())}
                        : out.names[at];
            const int size =
                std::snprintf(out.ids[at], sizeof(out.ids[at]), "mock_%llu_%u",
                              static_cast<unsigned long long>(request.attempt.request_id), at);
            if (size <= 0 || static_cast<size_t>(size) >= sizeof(out.ids[at])) {
                out.status = AYNI_CAPACITY_EXCEEDED;
                return;
            }
            call.protocol_id = {out.ids[at], static_cast<uint32_t>(size)};
            call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
            if (generic) {
                constexpr uint32_t envelope_nodes = 3;
                if (out.node_count > AYNI_VALUE_NODE_COUNT_MAX - envelope_nodes) {
                    out.status = AYNI_CAPACITY_EXCEEDED;
                    return;
                }
                auto &nodes = out.wrapped[at];
                nodes[0].kind = AYNI_ARGUMENT_OBJECT;
                nodes[0].subtree_end = out.node_count + envelope_nodes;
                nodes[1].kind = AYNI_ARGUMENT_STRING;
                nodes[1].subtree_end = 2;
                nodes[1].name = {"binding_id", sizeof("binding_id") - 1U};
                nodes[1].value.text = out.names[at];
                nodes[2].kind = AYNI_ARGUMENT_NUMBER;
                nodes[2].subtree_end = envelope_nodes;
                nodes[2].name = {"binding_version", sizeof("binding_version") - 1U};
                nodes[2].value.text = {"1", 1};
                for (uint32_t node = 0; node < out.node_count; ++node) {
                    nodes[envelope_nodes + node] = out.nodes[node];
                    nodes[envelope_nodes + node].subtree_end += envelope_nodes;
                }
                nodes[envelope_nodes].name = {"arguments", sizeof("arguments") - 1U};
                call.input.arguments = {nodes.data(), out.node_count + envelope_nodes};
            } else {
                call.input.arguments = {out.nodes.data(), out.node_count};
            }
        }
        uint32_t bytes = 0;
        out.status = ayni_tool_calls_measure(out.calls.data(), out.count, &bytes);
        if (out.status == AYNI_OK && bytes > result_limit) {
            out.status = AYNI_CAPACITY_EXCEEDED;
        }
    }
};
ToolPlanner::ToolPlanner() = default;
ToolPlanner::~ToolPlanner() = default;
ayni_status_t ToolPlanner::configure(uint32_t capacity, uint32_t text_limit,
                                     uint32_t result_limit) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    std::unique_ptr<Impl> created(new (std::nothrow) Impl);
    if (!created) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    created->text_limit = text_limit;
    created->result_limit = result_limit;
    created->pool_size = yyjson_read_max_memory_usage(text_limit, YYJSON_READ_NUMBER_AS_RAW);
    created->plans.reset(new (std::nothrow) Impl::Plan[capacity]{});
    created->json_bytes.reset(new (std::nothrow) char[capacity * text_limit]{});
    created->pool.reset(new (std::nothrow) char[created->pool_size]);
    if (!created->plans || !created->json_bytes || !created->pool) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    impl_ = std::move(created);
    return AYNI_OK;
}
void ToolPlanner::prepare(uint32_t index, const ayni_provider_request_v1 &request,
                          ayni_string_v1 body) {
    clear(index);
    impl_->plan(index, request, body);
}
ToolAnswer ToolPlanner::answer(uint32_t index) {
    const auto &plan = impl_->plans[index];
    return {plan.status, plan.count,
            plan.status == AYNI_OK && plan.count ? plan.calls.data() : nullptr};
}
void ToolPlanner::clear(uint32_t index) {
    auto &plan = impl_->plans[index];
    erase(impl_->json_bytes.get() + index * impl_->text_limit, plan.used);
    plan = {};
}
} // namespace ayni::providers::mock
