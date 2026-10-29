#include "catalog/capability/value_validation.h"

#include <array>
#include <cstring>
#include <limits>

namespace ayni::catalog {
namespace {
constexpr uint32_t no_node = std::numeric_limits<uint32_t>::max();
constexpr uint32_t work_capacity = AYNI_VALUE_NODE_COUNT_MAX + AYNI_VALUE_DEPTH_MAX;

// One explicit traversal action; a close action fills its container's final
// end index after all children. Inputs/defaults are only borrowed until publish.
struct Pending {
    uint32_t schema;            // Valid schema node, ignored by a close action.
    const ayni_value_v1 *input; // Original input or a schema-owned default.
    uint32_t node;              // Input root in that complete tree.
    uint32_t field;             // Output identity; zero for root/list elements.
    uint32_t depth;             // Root depth is one; bounded before scheduling.
    uint32_t close = no_node;   // Container output index, or no_node for a value.
};

// Sole normalization plan: no caller buffer is touched until every semantic
// check, default, byte sum and node capacity has succeeded (ADR-0084).
class Plan {
  public:
    explicit Plan(const ayni_schema_v1 &schema) : schema_(schema) {}
    ayni_status_t build(const ayni_value_v1 &input, uint32_t root);
    ayni_status_t publish(ayni_value_buffer_v1 &out) const;
    void borrow(DecodedValue &out) const {
        out.nodes = nodes_;
        out.count = count_;
    }

  private:
    bool push(Pending item);
    ayni_status_t expand(const Pending &item);
    ayni_status_t object(const Pending &item);
    ayni_status_t list(const Pending &item);
    bool append(const Pending &item);
    uint32_t find_field(const ayni_value_v1 &input, uint32_t parent, uint32_t field) const;

    const ayni_schema_v1 &schema_; // Admitted static schema, outlives plan/publish.
    std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes_{}; // Candidate only.
    uint32_t count_ = 0; // Occupied candidate nodes; zero only before root expansion.
    uint32_t bytes_ = 0; // Complete UTF-8 requirement, checked before each addition.
    std::array<Pending, work_capacity> pending_{}; // Explicit bounded traversal stack.
    uint32_t pending_count_ = 0;                   // Pending actions, not tree depth.
};

bool Plan::push(Pending item) {
    if (pending_count_ == pending_.size() || item.depth > AYNI_VALUE_DEPTH_MAX) {
        return false;
    }
    pending_[pending_count_++] = item;
    return true;
}

// Only active primitive data is copied. No padding, inactive union or caller
// subtree offset becomes part of the canonical output.
bool Plan::append(const Pending &item) {
    if (count_ == nodes_.size()) {
        return false;
    }
    const auto &source = item.input->nodes[item.node];
    auto &node = nodes_[count_++];
    node = {};
    node.kind = source.kind;
    node.field_id = item.field;
    node.subtree_end = count_;
    switch (node.kind) {
    case AYNI_VALUE_BOOL:
        node.value.boolean = source.value.boolean;
        break;
    case AYNI_VALUE_I64:
        node.value.integer = source.value.integer;
        break;
    case AYNI_VALUE_U64:
        node.value.unsigned_integer = source.value.unsigned_integer;
        break;
    case AYNI_VALUE_FLOAT64:
        node.value.approximate = source.value.approximate == 0.0 ? 0.0 : source.value.approximate;
        break;
    case AYNI_VALUE_DECIMAL:
        node.value.decimal = source.value.decimal;
        break;
    case AYNI_VALUE_TEXT:
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        if (source.value.text.size > AYNI_VALUE_BYTES_MAX - bytes_) {
            return false;
        }
        node.value.text = source.value.text;
        bytes_ += source.value.text.size;
        break;
    default:
        break;
    }
    return true;
}

uint32_t Plan::find_field(const ayni_value_v1 &input, uint32_t parent, uint32_t field) const {
    for (uint32_t at = parent + 1U; at < input.nodes[parent].subtree_end;
         at = input.nodes[at].subtree_end) {
        if (input.nodes[at].field_id == field) {
            return at;
        }
    }
    return no_node;
}

// Schedule declared fields in stable ID order, inserting their static defaults.
// A second bounded scan rejects unknown input fields rather than dropping them.
ayni_status_t Plan::object(const Pending &item) {
    std::array<uint32_t, AYNI_SCHEMA_NODE_COUNT_MAX> fields{};
    uint32_t count = 0;
    for (uint32_t at = item.schema + 1U; at < schema_.nodes[item.schema].subtree_end;
         at = schema_.nodes[at].subtree_end) {
        fields[count++] = at;
    }
    for (uint32_t at = item.node + 1U; at < item.input->nodes[item.node].subtree_end;
         at = item.input->nodes[at].subtree_end) {
        bool found = false;
        for (uint32_t field = 0; field < count; ++field) {
            found =
                found || schema_.nodes[fields[field]].field_id == item.input->nodes[at].field_id;
        }
        if (!found) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    // At most SCHEMA_NODE_COUNT_MAX entries; insertion order uses no library
    // allocation or comparator callback and keeps the work bound explicit.
    for (uint32_t at = 1; at < count; ++at) {
        const uint32_t selected = fields[at];
        uint32_t position = at;
        while (position != 0U &&
               schema_.nodes[fields[position - 1U]].field_id > schema_.nodes[selected].field_id) {
            fields[position] = fields[position - 1U];
            --position;
        }
        fields[position] = selected;
    }
    while (count != 0U) {
        const uint32_t schema = fields[--count];
        const auto &field = schema_.nodes[schema];
        const uint32_t node = find_field(*item.input, item.node, field.field_id);
        if (node != no_node) {
            if (!push({schema, item.input, node, field.field_id, item.depth + 1U})) {
                return AYNI_CAPACITY_EXCEEDED;
            }
        } else if (field.default_value != nullptr) {
            if (!push({schema, field.default_value, 0, field.field_id, item.depth + 1U})) {
                return AYNI_CAPACITY_EXCEEDED;
            }
        } else if ((field.flags & AYNI_SCHEMA_REQUIRED) != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return AYNI_OK;
}

ayni_status_t Plan::list(const Pending &item) {
    std::array<uint32_t, AYNI_VALUE_LIST_COUNT_MAX> elements{};
    uint32_t count = 0;
    for (uint32_t at = item.node + 1U; at < item.input->nodes[item.node].subtree_end;
         at = item.input->nodes[at].subtree_end) {
        elements[count++] = at;
    }
    const auto &schema = schema_.nodes[item.schema];
    if (count < schema.min_count || count > schema.max_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    while (count != 0U) {
        if (!push({item.schema + 1U, item.input, elements[--count], 0, item.depth + 1U})) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    return AYNI_OK;
}

ayni_status_t Plan::expand(const Pending &item) {
    const auto &source = item.input->nodes[item.node];
    const auto &schema = schema_.nodes[item.schema];
    if (source.kind == AYNI_VALUE_NULL || !value_container(source.kind)) {
        if (!scalar_matches(schema, source)) {
            return AYNI_INVALID_ARGUMENT;
        }
        return append(item) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
    }
    if (source.kind != schema.kind) {
        return AYNI_INVALID_ARGUMENT;
    }
    const uint32_t container = count_;
    if (!append(item) || !push({0, nullptr, 0, 0, item.depth, container})) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return source.kind == AYNI_VALUE_OBJECT ? object(item) : list(item);
}

ayni_status_t Plan::build(const ayni_value_v1 &input, uint32_t root) {
    static_cast<void>(push({root, &input, 0, 0, 1}));
    while (pending_count_ != 0U) {
        const Pending item = pending_[--pending_count_];
        if (item.close != no_node) {
            nodes_[item.close].subtree_end = count_;
            continue;
        }
        const auto status = expand(item);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}

ayni_status_t Plan::publish(ayni_value_buffer_v1 &out) const {
    if ((out.node_capacity != 0U && out.nodes == nullptr) ||
        (out.byte_capacity != 0U && out.bytes == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (count_ > out.node_capacity || bytes_ > out.byte_capacity) {
        out.node_count = count_;
        out.byte_count = bytes_;
        return AYNI_BUFFER_TOO_SMALL;
    }
    uint32_t used = 0;
    for (uint32_t at = 0; at < count_; ++at) {
        auto node = nodes_[at];
        if (node.kind == AYNI_VALUE_TEXT || node.kind == AYNI_VALUE_ENUM ||
            node.kind == AYNI_VALUE_IDENTIFIER) {
            if (node.value.text.size != 0U) {
                std::memcpy(out.bytes + used, node.value.text.data, node.value.text.size);
            }
            node.value.text.data = out.bytes == nullptr ? nullptr : out.bytes + used;
            used += node.value.text.size;
        }
        out.nodes[at] = node;
    }
    out.node_count = count_;
    out.byte_count = used;
    return AYNI_OK;
}
} // namespace

ayni_status_t validate_schema_defaults(const ayni_schema_v1 &schema) {
    // Admit every default's shape before any plan can reach a nested default.
    for (uint32_t at = 0; at < schema.node_count; ++at) {
        const auto *value = schema.nodes[at].default_value;
        if (value != nullptr) {
            const auto checked = validate_value(*value);
            if (checked != AYNI_OK) {
                return checked;
            }
        }
    }
    for (uint32_t at = 0; at < schema.node_count; ++at) {
        const auto *value = schema.nodes[at].default_value;
        if (value != nullptr) {
            Plan plan(schema);
            const auto normalized = plan.build(*value, at);
            if (normalized != AYNI_OK) {
                return normalized;
            }
        }
    }
    return AYNI_OK;
}

ayni_status_t normalize_value(const ayni_schema_v1 &schema, const ayni_value_v1 &input,
                              ayni_value_buffer_v1 &output) {
    Plan plan(schema);
    const auto status = plan.build(input, 0);
    return status == AYNI_OK ? plan.publish(output) : status;
}

ayni_status_t normalize_borrowed_value(const ayni_schema_v1 &schema, const ayni_value_v1 &input,
                                       DecodedValue &output) {
    Plan plan(schema);
    const auto status = plan.build(input, 0);
    if (status == AYNI_OK) {
        plan.borrow(output);
    }
    return status;
}

ayni_status_t read_normalized_value(const ayni_schema_v1 &schema, ayni_bytes_v1 body,
                                    DecodedValue &out) {
    DecodedValue candidate;
    const auto decoded = decode_value(body, candidate);
    if (decoded != AYNI_OK) {
        return decoded;
    }
    DecodedValue normalized;
    const auto status = normalize_borrowed_value(schema, candidate.view(), normalized);
    if (status != AYNI_OK) {
        return status;
    }
    if (!same_value(candidate.view(), normalized.view())) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = candidate;
    return AYNI_OK;
}

ayni_status_t normalize_schema_default(const ayni_schema_v1 &schema, uint32_t field,
                                       ayni_value_buffer_v1 &output) {
    const auto *value = schema.nodes[field].default_value;
    if (value == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    Plan plan(schema);
    const auto status = plan.build(*value, field);
    return status == AYNI_OK ? plan.publish(output) : status;
}
} // namespace ayni::catalog

extern "C" ayni_status_t ayni_value_normalize(const ayni_schema_v1 *schema,
                                              const ayni_value_v1 *input,
                                              ayni_value_buffer_v1 *output) {
    if (schema == nullptr || input == nullptr || output == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto status = ayni::catalog::validate_schema(*schema);
    if (status != AYNI_OK) {
        return status;
    }
    status = ayni::catalog::validate_value(*input);
    return status == AYNI_OK ? ayni::catalog::normalize_value(*schema, *input, *output) : status;
}
