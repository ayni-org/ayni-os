#include "authority/scope/scope.h"

#include "catalog/capability/field_domain.h"

namespace ayni::authority {
namespace {

using catalog::DomainField;
using catalog::DomainReader;
using catalog::FieldValues;

bool same_type(const FieldValues &left, const FieldValues &right) {
    return left.kind == right.kind && catalog::compare_tokens(left.unit, right.unit) == 0;
}

bool value_covers(const FieldValues &parent, const FieldValues &child) {
    if (!same_type(parent, child)) {
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

ayni_status_t scope_form(const ayni_scope_v1 &scope) {
    if (scope.struct_size != sizeof(scope)) {
        return scope.struct_size < sizeof(scope) ? AYNI_INVALID_ARGUMENT : AYNI_UNSUPPORTED_VERSION;
    }
    if (scope.field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
        (scope.field_count != 0U && scope.fields == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[index];
        if (field.struct_size != sizeof(field)) {
            return field.struct_size < sizeof(field) ? AYNI_INVALID_ARGUMENT
                                                     : AYNI_UNSUPPORTED_VERSION;
        }
        if (field.field_id == 0U) {
            return AYNI_INVALID_ARGUMENT;
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

ayni_status_t selected_values(const DomainField &domain, const ayni_scope_v1 &scope, bool actual,
                              FieldValues &out, bool &provided) {
    const auto *field = find_field(scope, domain.id);
    provided = field != nullptr;
    if (field == nullptr) {
        if (actual) {
            return AYNI_INVALID_ARGUMENT;
        }
        out = domain.values;
        return AYNI_OK;
    }
    const auto status = catalog::read_field_values(field->allowed, out);
    if (status != AYNI_OK) {
        return status;
    }
    return value_covers(domain.values, out) && (!actual || singleton(out)) ? AYNI_OK
                                                                           : AYNI_INVALID_ARGUMENT;
}

// Validation and size calculation never touch output. Only after every
// field succeeds may a second pass write the canonical record.
ayni_status_t inspect(ayni_bytes_v1 domain, const ayni_scope_v1 &scope, bool actual,
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
        const auto selected_status = selected_values(field, scope, actual, selected, provided);
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

Coverage compare_typed(ayni_bytes_v1 domain, const ayni_scope_v1 &parent,
                       const ayni_scope_v1 &child, bool actual) {
    uint32_t ignored = 0;
    auto status = inspect(domain, parent, false, ignored);
    if (status == AYNI_OK) {
        status = inspect(domain, child, actual, ignored);
    }
    if (status != AYNI_OK) {
        return {status, false};
    }
    DomainReader reader(domain);
    DomainField field{};
    bool covered = true;
    while (reader.next(field)) {
        FieldValues parent_values{};
        FieldValues child_values{};
        bool provided = false;
        static_cast<void>(selected_values(field, parent, false, parent_values, provided));
        static_cast<void>(selected_values(field, child, actual, child_values, provided));
        covered = value_covers(parent_values, child_values) && covered;
    }
    return {AYNI_OK, covered};
}

} // namespace

ayni_status_t normalize_scope(ayni_bytes_v1 domain, const ayni_scope_v1 &scope, uint8_t *out,
                              uint32_t capacity, uint32_t &out_size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t needed = 0;
    const auto status = inspect(domain, scope, false, needed);
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
        FieldValues values{};
        bool provided = false;
        static_cast<void>(selected_values(field, scope, false, values, provided));
        field.values = values;
        catalog::write_domain_field(field, out + at);
        at += catalog::encoded_field_size(field);
    }
    out_size = needed;
    return AYNI_OK;
}

Coverage covers(ayni_bytes_v1 domain, const ayni_scope_v1 &parent, const ayni_scope_v1 &child) {
    return compare_typed(domain, parent, child, false);
}

Coverage contains_request(ayni_bytes_v1 domain, const ayni_scope_v1 &allowed,
                          const ayni_scope_v1 &actual) {
    return compare_typed(domain, allowed, actual, true);
}

Coverage covers(ayni_bytes_v1 domain, ayni_bytes_v1 parent, ayni_bytes_v1 child) {
    auto status = inspect_encoded(domain, parent);
    if (status == AYNI_OK) {
        status = inspect_encoded(domain, child);
    }
    if (status != AYNI_OK) {
        return {status, false};
    }
    DomainReader parent_reader(parent);
    DomainReader child_reader(child);
    DomainField parent_field{};
    DomainField child_field{};
    bool covered = true;
    while (parent_reader.next(parent_field)) {
        static_cast<void>(child_reader.next(child_field));
        covered = value_covers(parent_field.values, child_field.values) && covered;
    }
    return {AYNI_OK, covered};
}

Coverage contains_request(ayni_bytes_v1 domain, ayni_bytes_v1 allowed,
                          const ayni_scope_v1 &actual) {
    auto status = inspect_encoded(domain, allowed);
    uint32_t ignored = 0;
    if (status == AYNI_OK) {
        status = inspect(domain, actual, true, ignored);
    }
    if (status != AYNI_OK) {
        return {status, false};
    }
    DomainReader reader(allowed);
    DomainField field{};
    bool covered = true;
    while (reader.next(field)) {
        FieldValues values{};
        // The schema membership was checked above; field metadata are the
        // same in every canonical scope of this exact capability version.
        const auto *input = find_field(actual, field.id);
        static_cast<void>(catalog::read_field_values(input->allowed, values));
        covered = value_covers(field.values, values) && covered;
    }
    return {AYNI_OK, covered};
}

} // namespace ayni::authority
