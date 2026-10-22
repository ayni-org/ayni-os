#include "kernel/command/command_canonical.h"

#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"
#include "catalog/catalog_id.h"
#include "durability/batch/record_body.h"
#include "foundation/digest.h"
#include "foundation/little_endian.h"

#include <array>
#include <cstring>

namespace ayni::kernel {
namespace {

// contracts/storage/records.md, raw command scope format 1. It has no
// catalog usage byte: raw requests cannot depend on current registration.
constexpr uint16_t raw_scope_format = 1;
constexpr uint32_t raw_scope_header_size = 4;
constexpr uint32_t raw_scope_count_at = 2;
constexpr uint32_t raw_scope_reserved_at = 3;
constexpr uint32_t raw_field_header_size = 8;
constexpr uint32_t raw_field_kind_at = 4;
constexpr uint32_t raw_field_unit_size_at = 5;
constexpr uint32_t raw_field_member_count_at = 6;
constexpr uint32_t raw_field_reserved_at = 7;
constexpr uint32_t integer_endpoint_size = 8;
constexpr uint32_t integer_pair_size = 2U * integer_endpoint_size;
constexpr uint32_t token_length_size = 1;
constexpr uint32_t known_flags =
    AYNI_GRANT_ALLOW_DELEGATION | AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA;
constexpr uint32_t fixed_without_id = authority_command_fixed_max - AYNI_CATALOG_ID_MAX_BYTES;
constexpr uint64_t signed_order_bit = uint64_t{1} << 63U;
using FieldOrder = std::array<uint32_t, AYNI_AUTHORITY_FIELD_COUNT_MAX>;

ayni_status_t check_size(uint32_t actual, uint32_t expected) {
    return actual == expected  ? AYNI_OK
           : actual < expected ? AYNI_INVALID_ARGUMENT
                               : AYNI_UNSUPPORTED_VERSION;
}

bool valid_scalars(const AuthorityCommandRecord &input) {
    return input.capability_version != 0U && catalog::CatalogId::is_valid(input.capability_id) &&
           input.subject_instance_id != 0U &&
           (input.parent_grant_id == 0U ? input.expected_revision == 0U
                                        : input.expected_revision != 0U) &&
           (input.flags & ~known_flags) == 0U &&
           ((input.lifetime == AYNI_GRANT_PERMANENT && input.duration_us == 0U) ||
            (input.lifetime == AYNI_GRANT_CURRENT_BOOT && input.duration_us != 0U));
}

ayni_status_t inspect(const CanonicalCommandView &input, FieldOrder &order,
                      uint32_t &out_scope_size) {
    if (input.grant == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &grant = *input.grant;
    auto status = check_size(grant.struct_size, sizeof(grant));
    if (status != AYNI_OK) {
        return status;
    }
    const AuthorityCommandRecord scalars{input.expected_revision,
                                         grant.capability_version,
                                         grant.capability_id,
                                         grant.subject_instance_id,
                                         grant.parent_grant_id,
                                         grant.lifetime,
                                         grant.flags,
                                         grant.duration_us,
                                         {}};
    if (!valid_scalars(scalars)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &scope = grant.scope;
    status = check_size(scope.struct_size, sizeof(scope));
    if (status != AYNI_OK) {
        return status;
    }
    if (scope.field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
        (scope.field_count != 0U && scope.fields == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t needed = raw_scope_header_size;
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[index];
        status = check_size(field.struct_size, sizeof(field));
        if (status != AYNI_OK) {
            return status;
        }
        if (field.field_id == 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        catalog::DomainField decoded{field.field_id, 0, {}};
        status = catalog::read_field_values(field.allowed, decoded.values);
        if (status != AYNI_OK) {
            return status;
        }
        // Both codecs have an eight-byte field header and identical value
        // widths. Sharing size calculation avoids divergent budget bounds.
        needed += catalog::encoded_field_size(decoded);
        uint32_t at = index;
        while (at > 0U && scope.fields[order[at - 1U]].field_id > field.field_id) {
            order[at] = order[at - 1U];
            --at;
        }
        if (at > 0U && scope.fields[order[at - 1U]].field_id == field.field_id) {
            return AYNI_INVALID_ARGUMENT;
        }
        order[at] = index;
    }
    out_scope_size = needed;
    return AYNI_OK;
}

void write_scope(const ayni_scope_v1 &scope, const FieldOrder &order, uint8_t *out) {
    foundation::store_le16(out, raw_scope_format);
    out[raw_scope_count_at] = static_cast<uint8_t>(scope.field_count);
    out[raw_scope_reserved_at] = 0;
    uint32_t at = raw_scope_header_size;
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[order[index]];
        catalog::FieldValues values{};
        static_cast<void>(catalog::read_field_values(field.allowed, values));
        foundation::store_le32(out + at, field.field_id);
        out[at + raw_field_kind_at] = static_cast<uint8_t>(values.kind);
        out[at + raw_field_unit_size_at] = static_cast<uint8_t>(values.unit.size);
        out[at + raw_field_member_count_at] = static_cast<uint8_t>(values.token_count);
        out[at + raw_field_reserved_at] = 0;
        at += raw_field_header_size;
        if (values.unit.size != 0U) {
            std::memcpy(out + at, values.unit.data, values.unit.size);
            at += values.unit.size;
        }
        if (values.kind == AYNI_AUTHORITY_I64) {
            foundation::store_le64(out + at, static_cast<uint64_t>(values.minimum));
            foundation::store_le64(out + at + integer_endpoint_size,
                                   static_cast<uint64_t>(values.maximum));
            at += integer_pair_size;
        } else {
            for (uint32_t member = 0; member < values.token_count; ++member) {
                const auto token = values.tokens[member];
                out[at++] = static_cast<uint8_t>(token.size);
                std::memcpy(out + at, token.data, token.size);
                at += token.size;
            }
        }
    }
}

// Numeric values remain unsigned wire bits here. Equality against the
// domain decoder is defined by converting its i64 endpoints modulo 2^64.
struct RawField {
    uint32_t id = 0;
    uint32_t kind = 0;
    ayni_string_v1 unit{};
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    std::array<ayni_string_v1, AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX> tokens{};
    uint32_t token_count = 0;
};

class RawScopeReader {
  public:
    explicit RawScopeReader(ayni_bytes_v1 body) : body_(body) {
        valid_ = body.data != nullptr && body.size >= raw_scope_header_size &&
                 foundation::load_le16(body.data) == raw_scope_format &&
                 body.data[raw_scope_count_at] <= AYNI_AUTHORITY_FIELD_COUNT_MAX &&
                 body.data[raw_scope_reserved_at] == 0U;
        if (valid_) {
            at_ = raw_scope_header_size;
            remaining_ = body.data[raw_scope_count_at];
        }
    }
    bool next(RawField &out) {
        if (!valid_ || remaining_ == 0U) {
            return false;
        }
        const uint8_t *bytes = nullptr;
        RawField field{};
        if (!take(raw_field_header_size, bytes)) {
            return false;
        }
        field.id = foundation::load_le32(bytes);
        field.kind = bytes[raw_field_kind_at];
        const uint32_t unit_size = bytes[raw_field_unit_size_at];
        field.token_count = bytes[raw_field_member_count_at];
        if (field.id <= previous_id_ || bytes[raw_field_reserved_at] != 0U ||
            unit_size > catalog::CatalogId::max_size || !take(unit_size, bytes)) {
            valid_ = false;
            return false;
        }
        field.unit = {reinterpret_cast<const char *>(bytes), unit_size};
        if (field.kind == AYNI_AUTHORITY_I64) {
            if (!catalog::CatalogId::is_valid(field.unit) || field.token_count != 0U ||
                !take(integer_pair_size, bytes)) {
                valid_ = false;
                return false;
            }
            field.minimum = foundation::load_le64(bytes);
            field.maximum = foundation::load_le64(bytes + integer_endpoint_size);
            // Sign-bit bias orders signed endpoints without conversion or
            // subtraction that could overflow at INT64_MIN/MAX.
            if ((field.minimum ^ signed_order_bit) > (field.maximum ^ signed_order_bit)) {
                valid_ = false;
                return false;
            }
        } else if (field.kind == AYNI_AUTHORITY_ENUM || field.kind == AYNI_AUTHORITY_IDENTIFIER) {
            if (unit_size != 0U || field.token_count == 0U ||
                field.token_count > AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX) {
                valid_ = false;
                return false;
            }
            for (uint32_t index = 0; index < field.token_count; ++index) {
                if (!take(token_length_size, bytes)) {
                    return false;
                }
                const uint32_t size = bytes[0];
                if (size > catalog::CatalogId::max_size || !take(size, bytes)) {
                    valid_ = false;
                    return false;
                }
                const ayni_string_v1 token{reinterpret_cast<const char *>(bytes), size};
                if (!catalog::CatalogId::is_valid(token) ||
                    (index != 0U &&
                     catalog::compare_tokens(field.tokens[index - 1U], token) >= 0)) {
                    valid_ = false;
                    return false;
                }
                field.tokens[index] = token;
            }
        } else {
            valid_ = false;
            return false;
        }
        previous_id_ = field.id;
        --remaining_;
        out = field;
        return true;
    }
    bool ok() const { return valid_ && remaining_ == 0U && at_ == body_.size; }

  private:
    bool take(uint32_t count, const uint8_t *&bytes) {
        if (!valid_ || count > body_.size - at_) {
            valid_ = false;
            return false;
        }
        bytes = body_.data + at_;
        at_ += count;
        return true;
    }
    ayni_bytes_v1 body_;
    uint32_t at_ = 0;
    uint32_t remaining_ = 0;
    uint32_t previous_id_ = 0;
    bool valid_ = false;
};

bool valid_scope(ayni_bytes_v1 body) {
    RawScopeReader reader(body);
    RawField field{};
    while (reader.next(field)) {
    }
    return reader.ok();
}

bool same_values(const catalog::FieldValues &left, const catalog::FieldValues &right) {
    if (left.kind != right.kind || catalog::compare_tokens(left.unit, right.unit) != 0) {
        return false;
    }
    if (left.kind == AYNI_AUTHORITY_I64) {
        return left.minimum == right.minimum && left.maximum == right.maximum;
    }
    if (left.token_count != right.token_count) {
        return false;
    }
    for (uint32_t index = 0; index < left.token_count; ++index) {
        if (catalog::compare_tokens(left.tokens[index], right.tokens[index]) != 0) {
            return false;
        }
    }
    return true;
}

bool same_values(const RawField &left, const catalog::FieldValues &right) {
    if (left.kind != right.kind || catalog::compare_tokens(left.unit, right.unit) != 0) {
        return false;
    }
    if (left.kind == AYNI_AUTHORITY_I64) {
        return left.minimum == static_cast<uint64_t>(right.minimum) &&
               left.maximum == static_cast<uint64_t>(right.maximum);
    }
    if (left.token_count != right.token_count) {
        return false;
    }
    for (uint32_t index = 0; index < left.token_count; ++index) {
        if (catalog::compare_tokens(left.tokens[index], right.tokens[index]) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

ayni_status_t measure_authority_command(const CanonicalCommandView &input, uint32_t &out_size) {
    FieldOrder order{};
    uint32_t scope_size = 0;
    const auto status = inspect(input, order, scope_size);
    if (status == AYNI_OK) {
        out_size = fixed_without_id + input.grant->capability_id.size + scope_size;
    }
    return status;
}

ayni_status_t encode_authority_command(const CanonicalCommandView &input, uint8_t *out,
                                       uint32_t capacity, uint32_t &out_size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    FieldOrder order{};
    uint32_t scope_size = 0;
    const auto status = inspect(input, order, scope_size);
    if (status != AYNI_OK) {
        return status;
    }
    const auto &grant = *input.grant;
    const uint32_t fixed_size = fixed_without_id + grant.capability_id.size;
    const uint32_t needed = fixed_size + scope_size;
    if (capacity < needed) {
        out_size = needed;
        return AYNI_BUFFER_TOO_SMALL;
    }
    durability::BodyWriter writer(out, fixed_size);
    writer.u32(AYNI_INTENT_GRANT_CREATE);
    writer.u32(input.expected_revision);
    writer.u32(grant.capability_version);
    writer.bytes(grant.capability_id.data, grant.capability_id.size);
    writer.u64(grant.subject_instance_id);
    writer.u64(grant.parent_grant_id);
    writer.u32(grant.lifetime);
    writer.u32(grant.flags);
    writer.u64(grant.duration_us);
    writer.u32(scope_size);
    write_scope(grant.scope, order, out + fixed_size);
    out_size = needed;
    return AYNI_OK;
}

bool read_authority_command(ayni_bytes_v1 bytes, AuthorityCommandRecord &out) {
    if (bytes.data == nullptr ||
        bytes.size > authority_command_max_bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX)) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    const uint32_t kind = reader.u32();
    AuthorityCommandRecord command{};
    command.expected_revision = reader.u32();
    command.capability_version = reader.u32();
    uint32_t id_size = 0;
    const auto *id = reader.bytes(catalog::CatalogId::max_size, id_size);
    command.capability_id = {reinterpret_cast<const char *>(id), id_size};
    command.subject_instance_id = reader.u64();
    command.parent_grant_id = reader.u64();
    command.lifetime = reader.u32();
    command.flags = reader.u32();
    command.duration_us = reader.u64();
    uint32_t scope_size = 0;
    const auto *scope = reader.bytes(bytes.size, scope_size);
    command.raw_scope = {scope, scope_size};
    if (!reader.ok() || kind != AYNI_INTENT_GRANT_CREATE || !valid_scalars(command) ||
        !valid_scope(command.raw_scope)) {
        return false;
    }
    out = command;
    return true;
}

bool authority_command_scope_matches(const AuthorityCommandRecord &command,
                                     ayni_bytes_v1 source_domain, ayni_bytes_v1 expanded_scope) {
    const auto valid = authority::covers(source_domain, source_domain, expanded_scope);
    if (valid.status != AYNI_OK || !valid.covered) {
        return false;
    }
    catalog::DomainReader domain(source_domain);
    catalog::DomainReader expanded(expanded_scope);
    RawScopeReader raw(command.raw_scope);
    RawField selected{};
    bool provided = raw.next(selected);
    catalog::DomainField registered{};
    catalog::DomainField stored{};
    while (domain.next(registered)) {
        if (!expanded.next(stored) || (provided && selected.id < registered.id)) {
            return false;
        }
        if (provided && selected.id == registered.id) {
            if (!same_values(selected, stored.values)) {
                return false;
            }
            provided = raw.next(selected);
        } else if (!same_values(registered.values, stored.values)) {
            return false;
        }
    }
    return domain.ok() && expanded.ok() && !provided && raw.ok();
}

uint64_t authority_command_digest(ayni_bytes_v1 bytes) {
    foundation::Digest digest;
    digest.add(bytes.data, bytes.size);
    return digest.value();
}

} // namespace ayni::kernel
