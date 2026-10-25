#include "authority/scene/scene_definition.h"
#include "authority/scope/scope.h"
#include "catalog/abi_checks.h"
#include "durability/batch/record_body.h"
#include <cstring>

namespace ayni::authority {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

struct RuleInput {
    catalog::CatalogId id; // Copied identity; domains and normalized Scopes borrow this build only.
    uint32_t version;
    ayni_bytes_v1 domain;
    ayni_bytes_v1 scope;
};
struct DefinitionInput {
    SceneDefinition definition{}; // Valid counts delimit the prepared arrays below.
    std::array<RuleInput, AYNI_SCENE_RULE_COUNT_MAX> rules{};
};
// Scene permission is a finite subset of each exact registered domain. Sorting
// makes equivalent declaration order independent without allocating a container.
ayni_status_t prepare_rules(const ayni_scene_desc_v1 &input,
                            const catalog::CapabilityRegistry &capabilities, uint32_t scope_bytes,
                            uint8_t *scratch, DefinitionInput &out) {
    for (uint32_t at = 0; at < input.rule_count; ++at) {
        const auto &source = input.rules[at];
        RuleInput rule{};
        if (!rule.id.assign(source.capability_id) || source.capability_version == 0 ||
            source.reserved != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        rule.version = source.capability_version;
        const auto found = capabilities.find(source.capability_id, rule.version);
        if (found.kind != catalog::CapabilityRegistry::Lookup::Kind::found) {
            return found.kind == catalog::CapabilityRegistry::Lookup::Kind::unknown_id
                       ? AYNI_NOT_FOUND
                       : AYNI_UNSUPPORTED_VERSION;
        }
        rule.domain = capabilities.domain_view(found.index);
        auto *normalized = scratch + static_cast<std::size_t>(at) * scope_bytes;
        uint32_t size = 0;
        const auto status =
            normalize_scope(rule.domain, source.allowed, normalized, scope_bytes, size);
        if (status != AYNI_OK) {
            return status == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : status;
        }
        rule.scope = {normalized, size};
        uint32_t insert = at;
        while (insert > 0 && out.rules[insert - 1].id.compare(rule.id) > 0) {
            out.rules[insert] = out.rules[insert - 1];
            --insert;
        }
        // A Scene selects one exact version per capability; duplicate names are ambiguous.
        if ((insert > 0 && out.rules[insert - 1].id.compare(rule.id) == 0) ||
            (insert < at && out.rules[insert].id.compare(rule.id) == 0)) {
            return AYNI_INVALID_ARGUMENT;
        }
        out.rules[insert] = rule;
    }
    return AYNI_OK;
}
bool valid_condition(const SceneCondition &condition) {
    const auto &source = condition.source;
    const auto &predicate = condition.predicate;
    return catalog::validate_source(source) == AYNI_OK &&
           source.origin == AYNI_OBSERVATION_MEASURED && source.id.equals(view(predicate.source)) &&
           source.version == predicate.version && predicate.minimum <= predicate.maximum &&
           predicate.max_age_us > 0 && predicate.max_age_us != UINT64_MAX &&
           (source.kind != AYNI_OBSERVATION_BOOL ||
            (predicate.minimum >= 0 && predicate.maximum <= 1));
}
ayni_status_t prepare_conditions(const ayni_scene_desc_v1 &input,
                                 const catalog::ObservationSources &sources, SceneDefinition &out) {
    for (uint32_t at = 0; at < input.condition_count; ++at) {
        SceneCondition condition{};
        auto status = catalog::decode_condition(input.conditions[at], condition.predicate);
        if (status != AYNI_OK) {
            return status;
        }
        uint32_t source_index = 0;
        if (!sources.find(view(condition.predicate.source), source_index)) {
            return AYNI_NOT_FOUND;
        }
        condition.source = sources.at(source_index);
        if (condition.source.version != condition.predicate.version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
        if (!valid_condition(condition)) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t insert = at;
        while (insert > 0 &&
               out.conditions[insert - 1].source.id.compare(condition.source.id) > 0) {
            out.conditions[insert] = out.conditions[insert - 1];
            --insert;
        }
        if (insert > 0 && out.conditions[insert - 1].source.id.compare(condition.source.id) == 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        out.conditions[insert] = condition;
    }
    return AYNI_OK;
}
uint32_t definition_size(const DefinitionInput &input) {
    uint32_t bytes = scene_definition_header_bytes;
    for (uint32_t at = 0; at < input.definition.rule_count; ++at) {
        const auto &rule = input.rules[at];
        bytes += scene_rule_fixed_bytes + rule.id.size() + rule.domain.size + rule.scope.size;
    }
    for (uint32_t at = 0; at < input.definition.condition_count; ++at) {
        const auto &source = input.definition.conditions[at].source;
        bytes += scene_condition_fixed_bytes + source.id.size() + source.unit.size();
    }
    return bytes;
}
// The one declaration writer also records byte offsets for runtime Scope checks.
void write_definition(DefinitionInput &input, uint8_t *bytes) {
    auto &definition = input.definition;
    definition.bytes = definition_size(input);
    durability::BodyWriter writer(bytes, definition.bytes);
    writer.u32(definition.rule_count);
    writer.u32(definition.condition_count);
    uint32_t offset = scene_definition_header_bytes;
    for (uint32_t at = 0; at < definition.rule_count; ++at) {
        const auto &rule = input.rules[at];
        writer.bytes(rule.id.data(), rule.id.size());
        writer.u32(rule.version);
        writer.bytes(rule.domain.data, rule.domain.size);
        writer.bytes(rule.scope.data, rule.scope.size);
        // records.md Scene rule: ID length, version and domain length precede domain bytes.
        constexpr uint32_t rule_domain_prefix_field_count = 3;
        constexpr uint32_t rule_domain_prefix = rule_domain_prefix_field_count * sizeof(uint32_t);
        const auto domain_offset = offset + rule_domain_prefix + rule.id.size();
        constexpr uint32_t length_prefix_bytes = sizeof(uint32_t);
        const auto scope_offset = domain_offset + rule.domain.size + length_prefix_bytes;
        definition.rules[at] = {rule.id,          rule.version, domain_offset,
                                rule.domain.size, scope_offset, rule.scope.size};
        offset += scene_rule_fixed_bytes + rule.id.size() + rule.domain.size + rule.scope.size;
    }
    for (uint32_t at = 0; at < definition.condition_count; ++at) {
        const auto &condition = definition.conditions[at];
        const auto &source = condition.source;
        writer.bytes(source.id.data(), source.id.size());
        writer.u32(source.version);
        writer.u32(source.kind);
        writer.u32(source.origin);
        writer.bytes(source.unit.data(), source.unit.size());
        writer.u64(source.validity_max_us);
        writer.u64(condition.predicate.max_age_us);
        writer.u64(static_cast<uint64_t>(condition.predicate.minimum));
        writer.u64(static_cast<uint64_t>(condition.predicate.maximum));
    }
}
bool read_id(durability::BodyReader &reader, catalog::CatalogId &id) {
    uint32_t size = 0;
    const auto *data = reader.bytes(catalog::CatalogId::max_size, size);
    return id.assign({reinterpret_cast<const char *>(data), size});
}
bool read_rule(durability::BodyReader &reader, ayni_bytes_v1 bytes, SceneRule &rule) {
    if (!read_id(reader, rule.capability)) {
        return false;
    }
    rule.version = reader.u32();
    const auto *domain = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, rule.domain_size);
    const auto *scope = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, rule.scope_size);
    if (domain == nullptr || scope == nullptr || rule.version == 0) {
        return false;
    }
    const auto coverage =
        covers({domain, rule.domain_size}, {domain, rule.domain_size}, {scope, rule.scope_size});
    if (coverage.status != AYNI_OK || !coverage.covered) {
        return false;
    }
    rule.domain_offset = static_cast<uint32_t>(domain - bytes.data);
    rule.scope_offset = static_cast<uint32_t>(scope - bytes.data);
    return true;
}
bool read_condition(durability::BodyReader &reader, SceneCondition &condition) {
    auto &source = condition.source;
    if (!read_id(reader, source.id)) {
        return false;
    }
    source.version = reader.u32();
    source.kind = reader.u32();
    source.origin = reader.u32();
    if (!read_id(reader, source.unit)) {
        return false;
    }
    source.validity_max_us = reader.u64();
    auto &predicate = condition.predicate;
    predicate.source = source.id;
    predicate.version = source.version;
    predicate.max_age_us = reader.u64();
    const auto minimum = reader.u64();
    const auto maximum = reader.u64();
    std::memcpy(&predicate.minimum, &minimum, sizeof(minimum));
    std::memcpy(&predicate.maximum, &maximum, sizeof(maximum));
    return valid_condition(condition);
}
} // namespace
uint32_t scene_definition_scratch_bytes(uint32_t scope_bytes) {
    return AYNI_SCENE_RULE_COUNT_MAX * scope_bytes + Scenes::definition_max_bytes(scope_bytes);
}
ayni_status_t build_scene_definition(const ayni_scene_desc_v1 &input,
                                     const catalog::CapabilityRegistry &capabilities,
                                     const catalog::ObservationSources &sources,
                                     uint32_t scope_bytes, uint8_t *scratch, SceneDefinition &out,
                                     ayni_bytes_v1 &bytes) {
    DefinitionInput prepared{};
    prepared.definition.id.assign(input.scene_id); // Registration already checked ABI/ID/counts.
    prepared.definition.version = input.version;
    prepared.definition.rule_count = input.rule_count;
    prepared.definition.condition_count = input.condition_count;
    const auto rules = prepare_rules(input, capabilities, scope_bytes, scratch, prepared);
    if (rules != AYNI_OK) {
        return rules;
    }
    const auto conditions = prepare_conditions(input, sources, prepared.definition);
    if (conditions != AYNI_OK) {
        return conditions;
    }
    auto *canonical = scratch + AYNI_SCENE_RULE_COUNT_MAX * scope_bytes;
    write_definition(prepared, canonical);
    out = prepared.definition;
    bytes = {canonical, out.bytes};
    return AYNI_OK;
}
// Restore checks canonical ordering and original semantic types; new configuration
// mismatches are consumption holds, never reinterpreted as malformed stored history.
bool read_scene_definition(ayni_bytes_v1 bytes, SceneDefinition &out) {
    if (bytes.data == nullptr || bytes.size < scene_definition_header_bytes) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    SceneDefinition value{};
    value.bytes = bytes.size;
    value.rule_count = reader.u32();
    value.condition_count = reader.u32();
    if (value.rule_count == 0 || value.rule_count > value.rules.size() ||
        value.condition_count > value.conditions.size()) {
        return false;
    }
    for (uint32_t at = 0; at < value.rule_count; ++at) {
        if (!read_rule(reader, bytes, value.rules[at]) ||
            (at > 0 && value.rules[at - 1].capability.compare(value.rules[at].capability) >= 0)) {
            return false;
        }
    }
    for (uint32_t at = 0; at < value.condition_count; ++at) {
        if (!read_condition(reader, value.conditions[at]) ||
            (at > 0 &&
             value.conditions[at - 1].source.id.compare(value.conditions[at].source.id) >= 0)) {
            return false;
        }
    }
    if (!reader.ok()) {
        return false;
    }
    out = value;
    return true;
}
} // namespace ayni::authority
