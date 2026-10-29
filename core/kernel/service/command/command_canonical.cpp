#include "kernel/service/command/command_canonical.h"

#include "authority/grant/grant_rules.h"
#include "authority/scope/scope.h"
#include "catalog/abi_checks.h"
#include "catalog/capability/field_domain.h"
#include "catalog/catalog_id.h"
#include "foundation/digest.h"
#include "foundation/little_endian.h"
#include "foundation/record_body.h"

namespace ayni::kernel {
namespace {

// contracts/storage/records.md, raw command scope format 1. It has no
// catalog usage byte: raw requests cannot depend on current registration.
constexpr uint32_t revoke_revision_at = 4;
constexpr uint32_t revoke_target_at = 8;
constexpr uint16_t raw_scope_format = 1;
constexpr uint32_t raw_scope_header_size = 4;
constexpr uint32_t raw_scope_count_at = 2;
constexpr uint32_t raw_scope_reserved_at = 3;
constexpr uint32_t raw_field_kind_at = 4;
constexpr uint32_t raw_field_unit_size_at = 5;
constexpr uint32_t raw_field_member_count_at = 6;
constexpr uint32_t raw_field_reserved_at = 7;
constexpr uint32_t fixed_without_id = authority_command_fixed_max - AYNI_CATALOG_ID_MAX_BYTES;
using catalog::FieldOrder;

bool valid_scalars(const AuthorityCommandRecord &input) {
    return input.capability_version != 0U && catalog::CatalogId::is_valid(input.capability_id) &&
           input.subject_instance_id != 0U &&
           (input.parent_grant_id == 0U ? input.expected_revision == 0U
                                        : input.expected_revision != 0U) &&
           authority::valid_grant_flags(input.flags) &&
           authority::valid_grant_lifetime(input.lifetime, input.duration_us);
}

// Validate the original request before consulting current authority state.
// Error order and omission identity remain independent of the live catalog.
ayni_status_t inspect(const CanonicalCommandView &input, FieldOrder &order,
                      uint32_t &out_scope_size) {
    if (input.grant == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &grant = *input.grant;
    auto status = catalog::check_struct_size(grant.struct_size, sizeof(grant), sizeof(grant));
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
    status = authority::check_scope_header(scope);
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t needed = raw_scope_header_size;
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[index];
        status = authority::check_scope_field_header(field);
        if (status != AYNI_OK) {
            return status;
        }
        catalog::DomainField decoded{field.field_id, 0, {}};
        status = catalog::read_field_values(field.allowed, decoded.values);
        if (status != AYNI_OK) {
            return status;
        }
        // Both codecs have an eight-byte field header and identical value
        // widths. Sharing size calculation avoids divergent budget bounds.
        needed += catalog::encoded_field_size(decoded);
        if (!catalog::insert_field_order(scope.fields, index, order)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    out_scope_size = needed;
    return AYNI_OK;
}

// inspect() checked every unchanged input and computed order/capacity. Only
// raw framing differs from a domain; canonical value payloads share one writer.
void write_scope(const ayni_scope_v1 &scope, const FieldOrder &order, uint8_t *out) {
    foundation::store_le16(out, raw_scope_format);
    out[raw_scope_count_at] = static_cast<uint8_t>(scope.field_count);
    out[raw_scope_reserved_at] = 0;
    uint32_t at = raw_scope_header_size;
    for (uint32_t index = 0; index < scope.field_count; ++index) {
        const auto &field = scope.fields[order[index]];
        const auto values = catalog::normalized_field_values(field.allowed);
        foundation::store_le32(out + at, field.field_id);
        out[at + raw_field_kind_at] = static_cast<uint8_t>(values.kind);
        out[at + raw_field_unit_size_at] = static_cast<uint8_t>(values.unit.size);
        out[at + raw_field_member_count_at] = static_cast<uint8_t>(values.token_count);
        out[at + raw_field_reserved_at] = 0;
        at += catalog::field_header_size;
        catalog::write_field_values(values, out + at);
        at += catalog::encoded_values_size(values);
    }
}

// Raw requests retain field IDs/omission, but borrow the same canonical value
// view as domains. There is no usage byte or catalog lookup in this framing.
struct RawField {
    uint32_t id = 0;
    catalog::FieldValues values{};
};

// Owns only raw-scope framing: version, reserved bytes, IDs and exact body end.
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
    // Validate raw ID/reserved framing, then let catalog validate the shared
    // payload. Publish only a complete field, preserving out on failure/end.
    bool next(RawField &out) {
        if (!valid_ || remaining_ == 0U) {
            return false;
        }
        const uint8_t *bytes = nullptr;
        RawField field{};
        if (!take(catalog::field_header_size, bytes)) {
            return false;
        }
        field.id = foundation::load_le32(bytes);
        uint32_t consumed = 0;
        if (field.id <= previous_id_ || bytes[raw_field_reserved_at] != 0U ||
            !catalog::read_encoded_values({body_.data + at_, body_.size - at_},
                                          bytes[raw_field_kind_at], bytes[raw_field_unit_size_at],
                                          bytes[raw_field_member_count_at], field.values,
                                          consumed)) {
            valid_ = false;
            return false;
        }
        at_ += consumed;
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

} // namespace

ayni_status_t encode_revoke_command(const CanonicalRevokeView &input, uint8_t *out,
                                    uint32_t capacity, uint32_t &out_size) {
    if (input.revoke == nullptr || (out == nullptr && capacity != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &revoke = *input.revoke;
    const auto status =
        catalog::check_struct_size(revoke.struct_size, sizeof(revoke), sizeof(revoke));
    if (status != AYNI_OK) {
        return status;
    }
    if (input.expected_revision == 0U || revoke.grant_id == 0U || revoke.reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capacity < revoke_command_size) {
        out_size = revoke_command_size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    foundation::store_le32(out, AYNI_INTENT_GRANT_REVOKE);
    foundation::store_le32(out + revoke_revision_at, input.expected_revision);
    foundation::store_le64(out + revoke_target_at, revoke.grant_id);
    out_size = revoke_command_size;
    return AYNI_OK;
}

bool read_revoke_command(ayni_bytes_v1 bytes, RevokeCommandRecord &out) {
    if (bytes.data == nullptr || bytes.size != revoke_command_size ||
        foundation::load_le32(bytes.data) != AYNI_INTENT_GRANT_REVOKE) {
        return false;
    }
    const RevokeCommandRecord record{foundation::load_le32(bytes.data + revoke_revision_at),
                                     foundation::load_le64(bytes.data + revoke_target_at)};
    if (record.expected_revision == 0U || record.target_grant_id == 0U) {
        return false;
    }
    out = record;
    return true;
}

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
    foundation::BodyWriter writer(out, fixed_size);
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
    foundation::BodyReader reader(bytes.data, bytes.size);
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
            if (!catalog::same_values(selected.values, stored.values)) {
                return false;
            }
            provided = raw.next(selected);
        } else if (!catalog::same_values(registered.values, stored.values)) {
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
