#include "authority/scope/scope.h"

#include "catalog/abi_checks.h"
#include "catalog/capability/field_domain.h"

namespace ayni::authority {

ayni_status_t check_scope_header(const ayni_scope_v1 &scope) {
    const auto status = catalog::check_struct_size(scope.struct_size, sizeof(scope), sizeof(scope));
    if (status != AYNI_OK) {
        return status;
    }
    return scope.field_count <= AYNI_AUTHORITY_FIELD_COUNT_MAX &&
                   (scope.field_count == 0U || scope.fields != nullptr)
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}

ayni_status_t check_scope_field_header(const ayni_scope_field_v1 &field) {
    const auto status = catalog::check_struct_size(field.struct_size, sizeof(field), sizeof(field));
    return status != AYNI_OK ? status : field.field_id != 0U ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}

namespace {

using catalog::DomainField;
using catalog::DomainReader;
using catalog::FieldValues;

// A constraint may omit fields/allow multiple values; a concrete request may not.
enum class Values { constraint, actual };

// C-AUTH-017: complete interval/set containment; canonical ordering makes
// token inclusion one bounded forward scan without combining independent Grants.
bool value_covers(const FieldValues &parent, const FieldValues &child) {
    if (!catalog::same_value_type(parent, child)) {
        return false;
    }
    if (parent.kind == AYNI_AUTHORITY_I64) {
        return parent.minimum <= child.minimum && child.maximum <= parent.maximum;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < child.token_count; ++index) {
        while (at < parent.token_count &&
               catalog::compare_tokens(parent.tokens[at], child.tokens[index]) < 0) {
            ++at;
        }
        if (at == parent.token_count ||
            catalog::compare_tokens(parent.tokens[at], child.tokens[index]) != 0) {
            return false;
        }
    }
    return true;
}

bool singleton(const FieldValues &values) {
    return values.kind == AYNI_AUTHORITY_I64 ? values.minimum == values.maximum
                                             : values.token_count == 1U;
}

// Keep ABI/framing checks ahead of value/domain checks, preserving classified
// errors for malformed or newer nested structures (C-VER-003, C-AUTH-017).
ayni_status_t scope_form(const ayni_scope_v1 &scope) {
    const auto status = check_scope_header(scope);
    if (status != AYNI_OK) {
        return status;
    }
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[index];
        const auto field_status = check_scope_field_header(field);
        if (field_status != AYNI_OK) {
            return field_status;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (scope.fields[previous].field_id == field.field_id) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    return AYNI_OK;
}

const ayni_scope_field_v1 *find_field(const ayni_scope_v1 &scope, uint32_t id) {
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        if (scope.fields[index].field_id == id) {
            return &scope.fields[index];
        }
    }
    return nullptr;
}

ayni_status_t selected_values(const DomainField &domain, const ayni_scope_v1 &scope, Values mode,
                              FieldValues &out, bool &provided) {
    const auto *field = find_field(scope, domain.id);
    provided = field != nullptr;
    if (field == nullptr) {
        if (mode == Values::actual) {
            return AYNI_INVALID_ARGUMENT;
        }
        out = domain.values;
        return AYNI_OK;
    }
    const auto status = catalog::read_field_values(field->allowed, out);
    if (status != AYNI_OK) {
        return status;
    }
    return value_covers(domain.values, out) && (mode == Values::constraint || singleton(out))
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}

// inspect() already checked type, range, completeness and ABI sizes. The second
// traversal needs the value view, not another status-producing admission pass.
FieldValues validated_values(const DomainField &domain, const ayni_scope_v1 &scope) {
    const auto *field = find_field(scope, domain.id);
    return field == nullptr ? domain.values : catalog::normalized_field_values(field->allowed);
}

// Validation and size calculation never touch output. Only after every
// field succeeds may a second pass write the canonical record.
ayni_status_t inspect(ayni_bytes_v1 domain, const ayni_scope_v1 &scope, Values mode,
                      uint32_t &size) {
    const auto status = scope_form(scope);
    if (status != AYNI_OK) {
        return status;
    }
    DomainReader reader(domain);
    DomainField field{};
    uint32_t provided_count = 0;
    uint32_t needed = catalog::field_domain_header_size;
    while (reader.next(field)) {
        FieldValues selected{};
        bool provided = false;
        const auto selected_status = selected_values(field, scope, mode, selected, provided);
        if (selected_status != AYNI_OK) {
            return selected_status;
        }
        provided_count += provided ? 1U : 0U;
        field.values = selected;
        needed += catalog::encoded_field_size(field);
    }
    if (!reader.ok() || provided_count != scope.field_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    size = needed;
    return AYNI_OK;
}

ayni_status_t inspect_encoded(ayni_bytes_v1 domain, ayni_bytes_v1 encoded) {
    DomainReader domain_reader(domain);
    DomainReader values_reader(encoded);
    DomainField field{};
    DomainField values{};
    while (domain_reader.next(field)) {
        if (!values_reader.next(values) || field.id != values.id || field.usage != values.usage ||
            !value_covers(field.values, values.values)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return domain_reader.ok() && values_reader.ok() ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}

// Both admission passes finish before comparison, preserving parent-before-child
// errors. Reuse bounded domain iteration without retaining a second field array.
Coverage compare_typed(ayni_bytes_v1 domain, const ayni_scope_v1 &parent,
                       const ayni_scope_v1 &child, Values mode) {
    uint32_t ignored = 0;
    auto status = inspect(domain, parent, Values::constraint, ignored);
    if (status == AYNI_OK) {
        status = inspect(domain, child, mode, ignored);
    }
    if (status != AYNI_OK) {
        return {status, false};
    }
    DomainReader reader(domain);
    DomainField field{};
    bool covered = true;
    while (reader.next(field)) {
        const auto parent_values = validated_values(field, parent);
        const auto child_values = validated_values(field, child);
        covered = value_covers(parent_values, child_values) && covered;
    }
    return {AYNI_OK, covered};
}

} // namespace

// C-AUTH-017: validate the entire scope before output; a capacity probe is
// successful admission and reports needed bytes without publishing a prefix.
ayni_status_t normalize_scope(ayni_bytes_v1 domain, const ayni_scope_v1 &scope, uint8_t *out,
                              uint32_t capacity, uint32_t &out_size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t needed = 0;
    const auto status = inspect(domain, scope, Values::constraint, needed);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < needed) {
        out_size = needed;
        return AYNI_BUFFER_TOO_SMALL;
    }
    DomainReader reader(domain);
    catalog::write_domain_header(out, reader.field_count());
    DomainField field{};
    uint32_t at = catalog::field_domain_header_size;
    while (reader.next(field)) {
        field.values = validated_values(field, scope);
        catalog::write_domain_field(field, out + at);
        at += catalog::encoded_field_size(field);
    }
    out_size = needed;
    return AYNI_OK;
}

Coverage covers(ayni_bytes_v1 domain, const ayni_scope_v1 &parent, const ayni_scope_v1 &child) {
    return compare_typed(domain, parent, child, Values::constraint);
}

Coverage contains_request(ayni_bytes_v1 domain, const ayni_scope_v1 &allowed,
                          const ayni_scope_v1 &actual) {
    return compare_typed(domain, allowed, actual, Values::actual);
}

// All three bodies are already encoded, so validate their shapes and domain
// relationships in the same bounded traversal that compares the scopes.
Coverage covers(ayni_bytes_v1 domain, ayni_bytes_v1 parent, ayni_bytes_v1 child) {
    DomainReader schema(domain);
    DomainReader parent_reader(parent);
    DomainReader child_reader(child);
    DomainField registered{};
    DomainField parent_field{};
    DomainField child_field{};
    bool covered = true;
    while (schema.next(registered)) {
        if (!parent_reader.next(parent_field) || !child_reader.next(child_field) ||
            registered.id != parent_field.id || registered.usage != parent_field.usage ||
            registered.id != child_field.id || registered.usage != child_field.usage ||
            !value_covers(registered.values, parent_field.values) ||
            !value_covers(registered.values, child_field.values)) {
            return {AYNI_INVALID_ARGUMENT, false};
        }
        covered = value_covers(parent_field.values, child_field.values) && covered;
    }
    return schema.ok() && parent_reader.ok() && child_reader.ok()
               ? Coverage{AYNI_OK, covered}
               : Coverage{AYNI_INVALID_ARGUMENT, false};
}

namespace {

// The stored parent is validated before the typed child, preserving error
// precedence. Omitted child fields still expand to the original domain.
Coverage compare_encoded_typed(ayni_bytes_v1 domain, ayni_bytes_v1 allowed,
                               const ayni_scope_v1 &child, Values mode) {
    auto status = inspect_encoded(domain, allowed);
    uint32_t ignored = 0;
    if (status == AYNI_OK) {
        status = inspect(domain, child, mode, ignored);
    }
    if (status != AYNI_OK) {
        return {status, false};
    }
    DomainReader reader(allowed);
    DomainReader schema(domain);
    DomainField field{};
    DomainField domain_field{};
    bool covered = true;
    while (reader.next(field)) {
        if (!schema.next(domain_field)) {
            return {AYNI_INVALID_ARGUMENT, false};
        }
        const auto values = validated_values(domain_field, child);
        covered = value_covers(field.values, values) && covered;
    }
    return {AYNI_OK, covered};
}

} // namespace

Coverage covers(ayni_bytes_v1 domain, ayni_bytes_v1 parent, const ayni_scope_v1 &child) {
    return compare_encoded_typed(domain, parent, child, Values::constraint);
}

Coverage contains_request(ayni_bytes_v1 domain, ayni_bytes_v1 allowed,
                          const ayni_scope_v1 &actual) {
    return compare_encoded_typed(domain, allowed, actual, Values::actual);
}

} // namespace ayni::authority
