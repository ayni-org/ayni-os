#include "authority/scope/business_request.h"
#include "catalog/capability/authority_projection.h"
#include "catalog/capability/field_domain.h"
namespace ayni::authority {
namespace {
bool routed(ayni_authority_field_usage_t usage) {
    return usage == AYNI_AUTHORITY_EXECUTION_TARGET || usage == AYNI_AUTHORITY_EGRESS_DESTINATION;
}
bool route_member(const catalog::Binding &binding, ayni_authority_field_usage_t usage,
                  ayni_string_v1 candidate) {
    return (usage == AYNI_AUTHORITY_EXECUTION_TARGET ? binding.endpoint : binding.destination)
        .equals(candidate);
}
// Canonical actual arguments are the only routing authority; execution consumes
// the same bytes. Containment proved singleton operands and exact domain types. Read
// only the route field's active token member; never reinterpret integer storage.
bool matches_route(const catalog::Binding &binding, ayni_bytes_v1 domain,
                   const ayni_scope_v1 &actual) {
    catalog::DomainReader reader(domain);
    catalog::DomainField field;
    while (reader.next(field)) {
        if (!routed(field.usage)) {
            continue;
        }
        bool found = false;
        for (uint32_t at = 0; at < actual.field_count; ++at) {
            const auto &operand = actual.fields[at];
            if (operand.field_id == field.id) {
                found =
                    route_member(binding, field.usage, operand.allowed.values.tokens.members[0]);
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return reader.ok();
}
} // namespace
bool binding_route_allowed(const catalog::Binding &binding, ayni_bytes_v1 scope) {
    catalog::DomainReader reader(scope);
    catalog::DomainField field;
    while (reader.next(field)) {
        if (!routed(field.usage)) {
            continue;
        }
        bool found = false;
        for (uint32_t at = 0; at < field.values.token_count; ++at) {
            found = found || route_member(binding, field.usage, field.values.tokens[at]);
        }
        if (!found) {
            return false;
        }
    }
    return reader.ok();
}
Coverage contains_business_request(const catalog::Binding &binding, ayni_bytes_v1 domain,
                                   const ayni_business_contract_v1 &business,
                                   const ayni_value_v1 &value, ayni_bytes_v1 scope) {
    const uint32_t rows = catalog::projection_rows(business, value);
    if (!rows) {
        return {AYNI_OK, false};
    }
    for (uint32_t at = 0; at < rows; ++at) {
        catalog::ProjectionRow row;
        const auto projected = catalog::project_row(business, value, at, row);
        if (projected != AYNI_OK) {
            return {projected, false};
        }
        const auto covered = contains_request(domain, scope, row.view());
        if (covered.status != AYNI_OK || !covered.covered) {
            return covered;
        }
        if (!matches_route(binding, domain, row.view())) {
            return {AYNI_OK, false};
        }
    }
    return {AYNI_OK, true};
}
} // namespace ayni::authority
