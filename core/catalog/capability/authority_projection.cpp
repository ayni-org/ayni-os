#include "catalog/capability/authority_projection.h"
#include "catalog/capability/value_validation.h"
#include "catalog/catalog_id.h"

#include <cstring>
#include <limits>

namespace ayni::catalog {
namespace {
constexpr uint32_t no_node = std::numeric_limits<uint32_t>::max();

bool same(ayni_string_v1 left, ayni_string_v1 right) {
    return compare_tokens(left, right) == 0;
}
bool contains_token(const FieldValues &domain, ayni_string_v1 token) {
    for (uint32_t at = 0; at < domain.token_count; ++at) {
        if (same(domain.tokens[at], token)) {
            return true;
        }
    }
    return false;
}

template <typename Node> uint32_t object_field(const Node *nodes, uint32_t parent, uint32_t id) {
    if (nodes[parent].kind != AYNI_VALUE_OBJECT) {
        return no_node;
    }
    for (uint32_t at = parent + 1U; at < nodes[parent].subtree_end; at = nodes[at].subtree_end) {
        if (nodes[at].field_id == id) {
            return at;
        }
    }
    return no_node;
}

// Every ancestor remains present/non-null for all valid business values.
// Optional with a default is present after normalization; nullable never proves
// this invariant because a caller may explicitly submit null instead.
ayni_status_t schema_leaf(const ayni_schema_v1 &schema, const ayni_authority_projection_v1 &path,
                          uint32_t &leaf, ProjectionShape &shape) {
    if (path.path_count == 0U || path.path_count > AYNI_VALUE_DEPTH_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t node = 0;
    bool list = false;
    for (uint32_t depth = 0; depth < path.path_count; ++depth) {
        const auto &parent = schema.nodes[node];
        if ((parent.flags & AYNI_SCHEMA_NULLABLE) != 0U) {
            return AYNI_UNSUPPORTED;
        }
        if (path.path[depth] == AYNI_PROJECTION_LIST_ITEM) {
            if (list || parent.kind != AYNI_VALUE_LIST || parent.min_count == 0U) {
                return AYNI_UNSUPPORTED;
            }
            if (shape.list_node && *shape.list_node != node) {
                return AYNI_UNSUPPORTED;
            }
            shape.list_node = node;
            list = true;
            ++node;
        } else {
            node = object_field(schema.nodes, node, path.path[depth]);
            if (node == no_node) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto &field = schema.nodes[node];
            if ((field.flags & AYNI_SCHEMA_REQUIRED) == 0U && field.default_value == nullptr) {
                return AYNI_UNSUPPORTED;
            }
        }
    }
    if (value_container(schema.nodes[node].kind) ||
        (schema.nodes[node].flags & AYNI_SCHEMA_NULLABLE) != 0U) {
        return AYNI_UNSUPPORTED;
    }
    leaf = node;
    return AYNI_OK;
}

// A path's exact numeric/token mapping does not depend on registration order.
bool leaf_projection(const ayni_schema_node_v1 &leaf,
                     const ayni_authority_projection_v1 &projection) {
    switch (leaf.kind) {
    case AYNI_VALUE_DECIMAL:
        return projection.flags == 0U && projection.scale == leaf.scale &&
               (leaf.scale == 0U ? same(leaf.unit, projection.unit)
                                 : !same(leaf.unit, projection.unit));
    case AYNI_VALUE_I64:
    case AYNI_VALUE_U64:
        return projection.flags == 0U && projection.scale == 0U &&
               same(leaf.unit, projection.unit) &&
               (leaf.kind != AYNI_VALUE_U64 ||
                leaf.max_u64 <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    case AYNI_VALUE_BOOL:
        return projection.flags == AYNI_PROJECTION_BOOL_I64 && projection.scale == 0U;
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        return projection.flags == 0U && projection.scale == 0U && projection.unit.size == 0U &&
               leaf.choice_count != 0U;
    default:
        return false;
    }
}

// The domain supplies finite authority bounds, not another business input.
bool leaf_matches(const DomainField &domain, const ayni_schema_node_v1 &leaf,
                  const ayni_authority_projection_v1 &projection) {
    if (!same(domain.values.unit, projection.unit) || !leaf_projection(leaf, projection)) {
        return false;
    }
    if (domain.values.kind == AYNI_AUTHORITY_I64) {
        int64_t minimum = 0, maximum = 0;
        if (leaf.kind == AYNI_VALUE_I64 || leaf.kind == AYNI_VALUE_DECIMAL) {
            minimum = leaf.min_i64;
            maximum = leaf.max_i64;
        } else if (leaf.kind == AYNI_VALUE_U64) {
            minimum = static_cast<int64_t>(leaf.min_u64);
            maximum = static_cast<int64_t>(leaf.max_u64);
        } else if (leaf.kind == AYNI_VALUE_BOOL) {
            maximum = 1;
        } else {
            return false;
        }
        return minimum >= domain.values.minimum && maximum <= domain.values.maximum;
    }
    if (domain.values.kind == AYNI_AUTHORITY_ENUM ? leaf.kind != AYNI_VALUE_ENUM
                                                  : leaf.kind != AYNI_VALUE_IDENTIFIER) {
        return false;
    }
    for (uint32_t at = 0; at < leaf.choice_count; ++at) {
        if (!contains_token(domain.values, leaf.choices[at])) {
            return false;
        }
    }
    return true;
}

uint32_t value_path(const ayni_value_v1 &value, const ayni_authority_projection_v1 &projection,
                    uint32_t steps, uint32_t row) {
    uint32_t node = 0;
    for (uint32_t depth = 0; depth < steps; ++depth) {
        if (projection.path[depth] == AYNI_PROJECTION_LIST_ITEM) {
            if (value.nodes[node].kind != AYNI_VALUE_LIST) {
                return no_node;
            }
            uint32_t at = node + 1U;
            for (uint32_t index = 0; index < row && at < value.nodes[node].subtree_end; ++index) {
                at = value.nodes[at].subtree_end;
            }
            if (at >= value.nodes[node].subtree_end) {
                return no_node;
            }
            node = at;
        } else {
            node = object_field(value.nodes, node, projection.path[depth]);
            if (node == no_node) {
                return no_node;
            }
        }
    }
    return node;
}

void fill_operand(const ayni_value_node_v1 &node, const ayni_authority_projection_v1 &projection,
                  uint32_t index, ProjectionRow &row) {
    auto &field = row.fields[index];
    field.struct_size = sizeof(field);
    field.field_id = projection.field_id;
    field.allowed.struct_size = sizeof(field.allowed);
    field.allowed.unit = projection.unit;
    if (node.kind == AYNI_VALUE_ENUM || node.kind == AYNI_VALUE_IDENTIFIER) {
        field.allowed.kind =
            node.kind == AYNI_VALUE_ENUM ? AYNI_AUTHORITY_ENUM : AYNI_AUTHORITY_IDENTIFIER;
        row.tokens[index] = node.value.text;
        field.allowed.values.tokens = {&row.tokens[index], 1, 0};
    } else {
        field.allowed.kind = AYNI_AUTHORITY_I64;
        const int64_t operand =
            node.kind == AYNI_VALUE_DECIMAL ? node.value.decimal.coefficient
            : node.kind == AYNI_VALUE_BOOL  ? static_cast<int64_t>(node.value.boolean)
            : node.kind == AYNI_VALUE_U64   ? static_cast<int64_t>(node.value.unsigned_integer)
                                            : node.value.integer;
        field.allowed.values.integer = {operand, operand};
    }
}
} // namespace

ayni_status_t validate_business_definition(const ayni_business_contract_v1 &business,
                                           ProjectionShape &out) {
    if (business.reserved != 0U || business.projection_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
        ((business.projection_count != 0U) != (business.projections != nullptr))) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto status = validate_schema(business.input);
    if (status != AYNI_OK) {
        return status;
    }
    status = validate_schema(business.result);
    if (status != AYNI_OK) {
        return status;
    }
    ProjectionShape shape;
    FieldOrder order{};
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        const auto &projection = business.projections[at];
        if (projection.field_id == 0U || projection.path_count == 0U ||
            projection.path_count > AYNI_VALUE_DEPTH_MAX ||
            projection.scale > AYNI_DECIMAL_SCALE_MAX ||
            (projection.unit.size != 0U && !CatalogId::is_valid(projection.unit)) ||
            !insert_field_order(business.projections, at, order)) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t leaf = 0;
        const auto path = schema_leaf(business.input, projection, leaf, shape);
        if (path != AYNI_OK) {
            return path;
        }
        if (!leaf_projection(business.input.nodes[leaf], projection)) {
            return AYNI_UNSUPPORTED;
        }
    }
    out = shape;
    return AYNI_OK;
}

ayni_status_t validate_projection(ayni_bytes_v1 domain, const ayni_business_contract_v1 &business,
                                  ProjectionShape &out) {
    const auto input = validate_schema(business.input);
    if (input != AYNI_OK) {
        return input;
    }
    const auto result = validate_schema(business.result);
    return result == AYNI_OK ? validate_projection_schemas_admitted(domain, business, out) : result;
}

ayni_status_t validate_projection_schemas_admitted(ayni_bytes_v1 domain,
                                                   const ayni_business_contract_v1 &business,
                                                   ProjectionShape &out) {
    if (business.reserved != 0U || business.projection_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
        ((business.projection_count != 0U) != (business.projections != nullptr))) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Reject malformed borrowed units before any byte comparison. These are
    // public definition operands, not trusted pointers from the catalog yet.
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        const auto &projection = business.projections[at];
        if (projection.field_id == 0U || projection.path_count == 0U ||
            projection.path_count > AYNI_VALUE_DEPTH_MAX ||
            projection.scale > AYNI_DECIMAL_SCALE_MAX ||
            (projection.flags != 0U && projection.flags != AYNI_PROJECTION_BOOL_I64) ||
            (projection.unit.size != 0U && !CatalogId::is_valid(projection.unit))) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    ProjectionShape shape;
    DomainReader reader(domain);
    DomainField field;
    uint32_t count = 0;
    while (reader.next(field)) {
        const ayni_authority_projection_v1 *projection = nullptr;
        for (uint32_t at = 0; at < business.projection_count; ++at) {
            if (business.projections[at].field_id == field.id) {
                if (projection != nullptr) {
                    return AYNI_INVALID_ARGUMENT;
                }
                projection = &business.projections[at];
            }
        }
        if (projection == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t leaf = 0;
        const auto status = schema_leaf(business.input, *projection, leaf, shape);
        if (status != AYNI_OK) {
            return status;
        }
        if (!leaf_matches(field, business.input.nodes[leaf], *projection)) {
            return AYNI_INVALID_ARGUMENT;
        }
        ++count;
    }
    if (!reader.ok() || count != business.projection_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = shape;
    return AYNI_OK;
}

ProjectionRow::ProjectionRow(const ProjectionRow &source) {
    *this = source;
}
ProjectionRow &ProjectionRow::operator=(const ProjectionRow &source) {
    if (this != &source) {
        fields = source.fields;
        tokens = source.tokens;
        count = source.count;
        for (uint32_t at = 0; at < count; ++at) {
            if (fields[at].allowed.kind != AYNI_AUTHORITY_I64) {
                fields[at].allowed.values.tokens.members = &tokens[at];
            }
        }
    }
    return *this;
}

uint32_t projection_rows(const ayni_business_contract_v1 &business, const ayni_value_v1 &value) {
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        const auto &projection = business.projections[at];
        uint32_t depth = 0;
        while (depth < projection.path_count &&
               projection.path[depth] != AYNI_PROJECTION_LIST_ITEM) {
            ++depth;
        }
        if (depth == projection.path_count) {
            continue;
        }
        const uint32_t list = value_path(value, projection, depth, 0);
        if (list == no_node) {
            return 0;
        }
        uint32_t count = 0;
        for (uint32_t child = list + 1U; child < value.nodes[list].subtree_end;
             child = value.nodes[child].subtree_end) {
            ++count;
        }
        return count;
    }
    return 1;
}

ayni_status_t project_row(const ayni_business_contract_v1 &business, const ayni_value_v1 &value,
                          uint32_t row, ProjectionRow &out) {
    if (business.projection_count > AYNI_AUTHORITY_FIELD_COUNT_MAX || value.node_count == 0U ||
        value.nodes == nullptr || row >= projection_rows(business, value)) {
        return AYNI_INVALID_ARGUMENT;
    }
    ProjectionRow candidate;
    candidate.count = business.projection_count;
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        const auto &projection = business.projections[at];
        const uint32_t leaf = value_path(value, projection, projection.path_count, row);
        if (leaf == no_node) {
            return AYNI_INVALID_ARGUMENT;
        }
        fill_operand(value.nodes[leaf], projection, at, candidate);
    }
    out = candidate;
    return AYNI_OK;
}
} // namespace ayni::catalog
