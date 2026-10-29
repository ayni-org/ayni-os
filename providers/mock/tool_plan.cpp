#include "tool_plan.h"
#include <algorithm>
#include <array>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <new>
#include <yyjson.h>
namespace ayni::providers::mock {
namespace {
constexpr char call_prefix[] = "call:";
constexpr uint32_t call_prefix_size = sizeof(call_prefix) - 1U;
constexpr char json_prefix[] = "json:";
constexpr uint32_t json_prefix_size = sizeof(json_prefix) - 1U;
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0;
}
void erase(char *bytes, size_t size) {
    auto *out = reinterpret_cast<volatile unsigned char *>(bytes);
    while (size--) {
        *out++ = 0;
    }
}
} // namespace
struct ToolPlanner::Impl {
    // One field of a planned call, copied from the offered tool at submit.
    struct Field {
        uint32_t id;
        ayni_value_kind_t kind;
        uint32_t scale;
        int64_t integer;
        uint32_t token_size;
        char token[AYNI_CATALOG_ID_MAX_BYTES];
    };
    struct Call {
        uint32_t version;
        uint32_t id_size;
        uint32_t text_limit;
        uint32_t field_count;
        uint32_t body_field;
        char id[AYNI_CATALOG_ID_MAX_BYTES];
        Field fields[AYNI_AUTHORITY_FIELD_COUNT_MAX];
    };
    // The tools service's decided answer for one slot; status OK or the model-output failure.
    struct Plan {
        ayni_status_t status;
        uint32_t count;
        uint32_t body_at; // Offset of the shared call body in the slot's text.
        Call calls[AYNI_CALL_TOOL_COUNT_MAX];
        ayni_string_v1 body{}; // Slot-owned immutable source text through clear.
        ayni_attempt_ref_v1 attempt{};
        bool named = false;
        uint32_t node_count = 0, used = 0;
        std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
    };
    uint32_t text_limit = 0, result_limit = 0;
    size_t pool_size = 0;
    std::unique_ptr<Plan[]> plans;
    std::unique_ptr<char[]> json_bytes, pool;
    ayni_tool_v1 tool{};
    ayni_tool_call_v1 posted[AYNI_CALL_TOOL_COUNT_MAX]{};
    static constexpr uint32_t proposal_nodes = AYNI_AUTHORITY_FIELD_COUNT_MAX + 2;
    std::array<ayni::ValueBuilder<proposal_nodes>, AYNI_CALL_TOOL_COUNT_MAX> posted_values{};
    char protocol_ids[AYNI_CALL_TOOL_COUNT_MAX][AYNI_MODEL_TOOL_ID_BYTES_MAX]{};
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
    // Copies what the answer needs from the offered tools: each leading
    // "call:<id>" word must name one, and the rest of the text is every body.
    void plan(uint32_t index, const ayni_provider_request_v1 &request, ayni_string_v1 body_text) {
        auto &out = plans[index];
        out.body = body_text;
        out.attempt = request.attempt;
        std::array<ayni_string_v1, AYNI_CALL_TOOL_COUNT_MAX> names{};
        uint32_t at = 0;
        while (body_text.size - at >= call_prefix_size &&
               std::memcmp(body_text.data + at, call_prefix, call_prefix_size) == 0) {
            if (out.count == names.size()) {
                out.status = AYNI_CAPACITY_EXCEEDED;
                return;
            }
            uint32_t end = at + call_prefix_size;
            while (end < body_text.size && body_text.data[end] != ' ') {
                ++end;
            }
            names[out.count++] = {body_text.data + at + call_prefix_size,
                                  end - at - call_prefix_size};
            at = end < body_text.size ? end + 1U : end;
        }
        out.body_at = at;
        out.named = out.count && body_text.size - at >= json_prefix_size &&
                    std::memcmp(body_text.data + at, json_prefix, json_prefix_size) == 0;
        for (uint32_t call = 0; call < out.count; ++call) {
            if (!copy_tool(request, names[call], out.calls[call], out.named) ||
                (!out.named && body_text.size - at > out.calls[call].text_limit)) {
                out.status = AYNI_INVALID_ARGUMENT;
                return;
            }
        }
        if (out.named) {
            const uint32_t begin = at + json_prefix_size;
            out.status = parse_json(index, body_text.data + begin, body_text.size - begin);
        }
    }

    // The flat deterministic fixture chooses a first admissible scalar; generic
    // nested fixtures use json: instead. Neither path implements business validation.
    static bool first_value(const ayni_schema_node_v1 &leaf,
                            const ayni_authority_value_set_v1 &allowed, Field &planned) {
        if (allowed.kind == AYNI_AUTHORITY_I64) {
            const int64_t lower =
                leaf.kind == AYNI_VALUE_BOOL
                    ? 0
                    : (leaf.kind == AYNI_VALUE_U64 ? static_cast<int64_t>(leaf.min_u64)
                                                   : leaf.min_i64);
            const int64_t upper =
                leaf.kind == AYNI_VALUE_BOOL
                    ? 1
                    : (leaf.kind == AYNI_VALUE_U64 ? static_cast<int64_t>(leaf.max_u64)
                                                   : leaf.max_i64);
            planned.integer = std::max(lower, allowed.values.integer.min_inclusive);
            return planned.integer <= std::min(upper, allowed.values.integer.max_inclusive);
        }
        for (uint32_t token = 0; token < allowed.values.tokens.member_count; ++token) {
            const auto candidate = allowed.values.tokens.members[token];
            bool member = leaf.kind == AYNI_VALUE_IDENTIFIER && !leaf.choice_count;
            for (uint32_t choice = 0; choice < leaf.choice_count; ++choice) {
                member = member || same(candidate, leaf.choices[choice]);
            }
            if (member && candidate.size <= sizeof(planned.token)) {
                planned.token_size = candidate.size;
                std::memcpy(planned.token, candidate.data, candidate.size);
                return true;
            }
        }
        return false;
    }
    static bool projected_field(const ayni_business_contract_v1 &definition,
                                const ayni_tool_v1 &offer, uint32_t field, Call &call) {
        const ayni_authority_projection_v1 *projection = nullptr;
        for (uint32_t at = 0; at < definition.projection_count; ++at) {
            if (definition.projections[at].field_id == offer.fields[field].field_id) {
                projection = &definition.projections[at];
                break;
            }
        }
        if (!projection || projection->path_count != 1) {
            return false;
        }
        const auto schema = definition.input;
        const ayni_schema_node_v1 *leaf = nullptr;
        for (uint32_t at = 1; at < schema.node_count; at = schema.nodes[at].subtree_end) {
            if (schema.nodes[at].field_id == projection->path[0]) {
                leaf = &schema.nodes[at];
                break;
            }
        }
        if (!leaf || leaf->kind == AYNI_VALUE_LIST || leaf->kind == AYNI_VALUE_OBJECT ||
            leaf->kind == AYNI_VALUE_TEXT || leaf->kind == AYNI_VALUE_FLOAT64) {
            return false;
        }
        for (uint32_t prior = 0; prior < field; ++prior) {
            if (call.fields[prior].id == leaf->field_id) {
                return false;
            }
        }
        auto &planned = call.fields[field];
        planned.id = leaf->field_id;
        planned.kind = leaf->kind;
        planned.scale = leaf->scale;
        return first_value(*leaf, offer.fields[field].domain, planned);
    }
    static ayni_status_t inspect_flat(const ayni_business_contract_v1 &definition,
                                      const ayni_tool_v1 &offer, Call &call) {
        const auto schema = definition.input;
        if (schema.nodes[0].kind == AYNI_VALUE_TEXT) {
            if (call.field_count) {
                return AYNI_UNSUPPORTED;
            }
            call.text_limit = schema.nodes[0].max_count;
            return AYNI_OK;
        }
        if (schema.nodes[0].kind != AYNI_VALUE_OBJECT) {
            return AYNI_UNSUPPORTED;
        }
        for (uint32_t field = 0; field < call.field_count; ++field) {
            if (!projected_field(definition, offer, field, call)) {
                return AYNI_UNSUPPORTED;
            }
        }
        bool body_found = false;
        for (uint32_t at = 1; at < schema.node_count; at = schema.nodes[at].subtree_end) {
            const auto &leaf = schema.nodes[at];
            if (leaf.kind == AYNI_VALUE_TEXT) {
                if (body_found) {
                    return AYNI_UNSUPPORTED;
                }
                body_found = true;
                call.body_field = leaf.field_id;
                call.text_limit = leaf.max_count;
            } else if ((leaf.flags & AYNI_SCHEMA_REQUIRED) && !leaf.default_value) {
                bool mapped = false;
                for (uint32_t field = 0; field < call.field_count; ++field) {
                    mapped = mapped || call.fields[field].id == leaf.field_id;
                }
                if (!mapped) {
                    return AYNI_UNSUPPORTED;
                }
            }
        }
        return body_found ? AYNI_OK : AYNI_UNSUPPORTED;
    }
    bool copy_tool(const ayni_provider_request_v1 &request, ayni_string_v1 name, Call &out,
                   bool named) {
        for (uint32_t index = 0; index < request.tool_count; ++index) {
            tool = {};
            tool.struct_size = sizeof(tool);
            if (request.tool(request.tool_context, index, &tool) != AYNI_OK ||
                !same(tool.binding_id, name)) {
                continue;
            }
            if (tool.field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
                tool.binding_id.size > sizeof(out.id)) {
                return false;
            }
            out = {};
            out.version = tool.binding_version;
            out.id_size = tool.binding_id.size;
            out.text_limit = tool.text_limit;
            out.field_count = tool.field_count;
            std::memcpy(out.id, tool.binding_id.data, tool.binding_id.size);
            if (named) {
                return true;
            }
            struct Inspection {
                const ayni_tool_v1 &offer;
                Call &call;
            } inspection{tool, out};
            return ayni_business_contract_read(
                       tool.original_domain, tool.business_contract, &inspection,
                       [](void *context, const ayni_business_contract_v1 *definition) {
                           auto &view = *static_cast<Inspection *>(context);
                           return inspect_flat(*definition, view.offer, view.call);
                       }) == AYNI_OK;
        }
        return false;
    }

    // Views of a slot's planned calls in the scratch above, for one post.
    const ayni_tool_call_v1 *calls_of(uint32_t index) {
        const auto &plan = plans[index];
        const ayni_string_v1 body_text{plan.body.data + plan.body_at,
                                       plan.body.size - plan.body_at};
        for (uint32_t call = 0; call < plan.count; ++call) {
            const auto &planned = plan.calls[call];
            auto &value = posted_values[call];
            value.clear();
            if (plan.named) {
                // Named source is mapped and authorized by the owner, never this fixture.
            } else if (planned.body_field) {
                value.object();
                for (uint32_t at = 0; at < planned.field_count; ++at) {
                    const auto &field = planned.fields[at];
                    switch (field.kind) {
                    case AYNI_VALUE_BOOL:
                        value.boolean(field.id, field.integer != 0);
                        break;
                    case AYNI_VALUE_I64:
                        value.integer(field.id, field.integer);
                        break;
                    case AYNI_VALUE_U64:
                        value.unsigned_integer(field.id, static_cast<uint64_t>(field.integer));
                        break;
                    case AYNI_VALUE_DECIMAL:
                        value.decimal(field.id, {field.integer, field.scale, 0});
                        break;
                    case AYNI_VALUE_ENUM:
                        value.enumeration(field.id, {field.token, field.token_size});
                        break;
                    case AYNI_VALUE_IDENTIFIER:
                        value.identifier(field.id, {field.token, field.token_size});
                        break;
                    default:
                        break; // Inspection rejects other projection kinds before planning.
                    }
                }
                value.text(planned.body_field, body_text);
                value.end();
            } else {
                value.text(0, body_text);
            }
            auto &out = posted[call];
            out = {};
            out.struct_size = sizeof(out);
            out.binding_version = planned.version;
            out.binding_id = {planned.id, planned.id_size};
            out.input_kind = plan.named ? AYNI_TOOL_INPUT_ARGUMENTS : AYNI_TOOL_INPUT_VALUE;
            const int id_size =
                std::snprintf(protocol_ids[call], sizeof(protocol_ids[call]), "mock_%llu_%u",
                              static_cast<unsigned long long>(plan.attempt.request_id), call);
            out.protocol_id = {protocol_ids[call], static_cast<uint32_t>(id_size)};
            if (id_size <= 0 || static_cast<size_t>(id_size) >= sizeof(protocol_ids[call])) {
                plans[index].status = AYNI_CAPACITY_EXCEEDED;
                return nullptr;
            }
            if (plan.named) {
                out.input.arguments = {plan.nodes.data(), plan.node_count};
            } else if (value.finish(out.input.value) != AYNI_OK) {
                plans[index].status = AYNI_CAPACITY_EXCEEDED;
                return nullptr;
            }
        }
        return posted;
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
    auto &plan = impl_->plans[index];
    if (plan.status != AYNI_OK || !plan.count) {
        return;
    }
    const auto *calls = impl_->calls_of(index);
    if (plan.status == AYNI_OK) {
        uint32_t size = 0;
        plan.status = ayni_tool_calls_measure(calls, plan.count, &size);
        if (plan.status == AYNI_OK && size > impl_->result_limit) {
            plan.status = AYNI_CAPACITY_EXCEEDED;
        }
    }
}
ToolAnswer ToolPlanner::answer(uint32_t index) {
    auto &plan = impl_->plans[index];
    const auto *calls = plan.status == AYNI_OK && plan.count ? impl_->calls_of(index) : nullptr;
    return {plan.status, plan.count, calls};
}
void ToolPlanner::clear(uint32_t index) {
    auto &plan = impl_->plans[index];
    erase(impl_->json_bytes.get() + index * impl_->text_limit, plan.used);
    plan = {};
    for (auto &value : impl_->posted_values) {
        value.clear();
    }
    std::memset(impl_->posted, 0, sizeof(impl_->posted));
}
} // namespace ayni::providers::mock
