// Accepted revoke identity and lifecycle belong to the command table. The
// restored result window is separate from these volatile controls so reload
// cannot forget an immediate deny accepted before storage admission.
#include "kernel/command/command_table.h"

#include <cstring>

namespace ayni::kernel {

CommandTable::ExactLookup CommandTable::find_exact(uint64_t principal_id, uint64_t command_id,
                                                   const CanonicalRevokeView &input) {
    staged_size_ = 0;
    std::array<uint8_t, revoke_command_size> bytes{};
    uint32_t size = 0;
    const auto status = encode_revoke_command(input, bytes.data(), revoke_command_size, size);
    if (status != AYNI_OK) {
        return {status, Lookup::unmapped, nullptr};
    }
    return find_revoke_bytes(principal_id, command_id, {bytes.data(), size});
}

CommandTable::ExactLookup
CommandTable::find_revoke_bytes(uint64_t principal_id, uint64_t command_id, ayni_bytes_v1 bytes) {
    if (principal_id == 0U || command_id == 0U) {
        return {AYNI_INVALID_ARGUMENT, Lookup::unmapped, nullptr};
    }
    const PendingRevoke *control = nullptr;
    const auto pending_lookup =
        find_pending(principal_id, command_id, AYNI_INTENT_GRANT_REVOKE, bytes, control);
    if (pending_lookup != Lookup::unmapped) {
        return {AYNI_OK, pending_lookup,
                pending_lookup == Lookup::retried ? mapped(principal_id, command_id) : nullptr,
                control};
    }
    const auto *existing = mapped(principal_id, command_id);
    const uint64_t digest = authority_command_digest(bytes);
    if (existing != nullptr) {
        const auto original = canonical(*existing);
        const bool equal = existing->kind == AYNI_INTENT_GRANT_REVOKE &&
                           existing->canonical_size == bytes.size && existing->digest == digest &&
                           std::memcmp(original.data, bytes.data, bytes.size) == 0;
        return {AYNI_OK, equal ? Lookup::retried : Lookup::conflict, equal ? existing : nullptr};
    }
    if (command_id <= floor(principal_id)) {
        return {AYNI_OK, Lookup::expired, nullptr};
    }
    if (bytes.size > canonical_max_bytes_) {
        return {AYNI_CAPACITY_EXCEEDED, Lookup::unmapped, nullptr};
    }
    std::memcpy(scratch_, bytes.data, bytes.size);
    staged_size_ = bytes.size;
    staged_principal_ = principal_id;
    staged_command_ = command_id;
    staged_digest_ = digest;
    staged_kind_ = AYNI_INTENT_GRANT_REVOKE;
    return {AYNI_OK, Lookup::unmapped, nullptr};
}

bool CommandTable::can_insert(uint64_t principal_id) const {
    if (principal_id == 0U || !has_row(principal_id)) {
        return false;
    }
    const Row *row = row_of(principal_id);
    if (row == nullptr || row->count < capacity_) {
        return true;
    }
    const auto *mappings = mappings_of(*row);
    for (uint32_t index = 0; index < row->count; ++index) {
        if (mappings[index].retention == CommandRetention::normal) {
            return true;
        }
    }
    return false;
}

CommandTable::Lookup CommandTable::find_pending(uint64_t principal_id, uint64_t command_id,
                                                ayni_intent_kind_t kind, ayni_bytes_v1 bytes,
                                                const PendingRevoke *&out_pending) const {
    const auto *control = pending(principal_id, command_id);
    if (control == nullptr) {
        return Lookup::unmapped;
    }
    if (kind != AYNI_INTENT_GRANT_REVOKE || bytes.data == nullptr ||
        bytes.size != revoke_command_size ||
        std::memcmp(control->canonical.data(), bytes.data, revoke_command_size) != 0) {
        return Lookup::conflict;
    }
    out_pending = control;
    return Lookup::retried;
}

PendingRevoke *CommandTable::pending_slot(uint64_t principal_id, uint64_t command_id) const {
    if (principal_id == 0U || command_id == 0U) {
        return nullptr;
    }
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        if (pending_[index].accepted.principal_id == principal_id &&
            pending_[index].accepted.command_id == command_id) {
            return pending_ + index;
        }
    }
    return nullptr;
}

const PendingRevoke *CommandTable::pending(uint64_t principal_id, uint64_t command_id) const {
    return pending_slot(principal_id, command_id);
}

const CommandMapping *CommandTable::mapped(uint64_t principal_id, uint64_t command_id) const {
    const Row *row = row_of(principal_id);
    if (row == nullptr) {
        return nullptr;
    }
    const auto *mappings = mappings_of(*row);
    for (uint32_t index = 0; index < row->count; ++index) {
        if (mappings[index].command_id == command_id) {
            return mappings + index;
        }
    }
    return nullptr;
}

ayni_status_t CommandTable::can_accept_revoke(const AcceptedRevoke &accepted) const {
    if (accepted.principal_id == 0U || accepted.command_id == 0U ||
        accepted.binding_revision == 0U || accepted.expected_revision == 0U ||
        accepted.target_grant_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto *existing = pending(accepted.principal_id, accepted.command_id);
    if (existing != nullptr) {
        const auto &prior = existing->accepted;
        return prior.target_grant_id == accepted.target_grant_id &&
                       prior.expected_revision == accepted.expected_revision &&
                       prior.binding_revision == accepted.binding_revision
                   ? AYNI_OK
                   : AYNI_IDEMPOTENCY_CONFLICT;
    }
    if (mapped(accepted.principal_id, accepted.command_id) != nullptr) {
        return AYNI_INVALID_STATE; // The caller must return the already mapped result.
    }
    if (accepted.command_id <= floor(accepted.principal_id)) {
        return AYNI_EXPIRED;
    }
    bool free = false;
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        if (pending_[index].accepted.principal_id == 0U) {
            free = true;
        } else if (pending_[index].accepted.target_grant_id == accepted.target_grant_id) {
            return AYNI_BUSY;
        }
    }
    return free ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

ayni_status_t CommandTable::accept_revoke(const AcceptedRevoke &accepted,
                                          const PendingRevoke *&out_pending) {
    const auto status = can_accept_revoke(accepted);
    if (status != AYNI_OK) {
        return status;
    }
    const auto *existing = pending(accepted.principal_id, accepted.command_id);
    if (existing != nullptr) {
        out_pending = existing;
        return AYNI_OK;
    }
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        if (pending_[index].accepted.principal_id != 0U) {
            continue;
        }
        PendingRevoke control{};
        control.accepted = accepted;
        const ayni_grant_revoke_v1 input{sizeof(ayni_grant_revoke_v1), 0, accepted.target_grant_id};
        uint32_t size = 0;
        if (encode_revoke_command({accepted.expected_revision, &input}, control.canonical.data(),
                                  revoke_command_size, size) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
        pending_[index] = control;
        staged_size_ = 0;
        out_pending = pending_ + index;
        return AYNI_OK;
    }
    return AYNI_CAPACITY_EXCEEDED;
}

bool CommandTable::owns_pending(const PendingRevoke &control) const {
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        if (pending_ + index == &control) {
            return control.accepted.principal_id != 0U;
        }
    }
    return false;
}

const PendingRevoke *CommandTable::next_pending_revoke() const {
    const PendingRevoke *first = nullptr;
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        const auto &control = pending_[index];
        if (control.accepted.principal_id != 0U && control.queued_commit_seq == 0U &&
            (first == nullptr ||
             control.accepted.target_grant_id < first->accepted.target_grant_id)) {
            first = &control;
        }
    }
    return first;
}

bool CommandTable::can_publish_accepted(const PendingRevoke &control) const {
    return owns_pending(control) && control.queued_commit_seq == 0U &&
           revoke_command_size <= canonical_max_bytes_ &&
           can_insert(control.accepted.principal_id) &&
           mapped(control.accepted.principal_id, control.accepted.command_id) == nullptr;
}

bool CommandTable::publish_accepted_revoke(const PendingRevoke &control, CommandMapping mapping,
                                           uint64_t boot_epoch) {
    if (!can_publish_accepted(control) || boot_epoch == 0U || mapping.commit_seq == 0U ||
        mapping.principal_id != control.accepted.principal_id ||
        mapping.command_id != control.accepted.command_id ||
        mapping.kind != AYNI_INTENT_GRANT_REVOKE) {
        return false;
    }
    const ayni_bytes_v1 bytes{control.canonical.data(), revoke_command_size};
    mapping.digest = authority_command_digest(bytes);
    mapping.canonical_size = revoke_command_size;
    mapping.retention = CommandRetention::accepted_revoke;
    if (!valid_authority_mapping(mapping, bytes)) {
        return false;
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    if (!add(*row, mapping, bytes)) {
        return false;
    }
    auto *published = pending_slot(mapping.principal_id, mapping.command_id);
    published->queued_boot_epoch = boot_epoch;
    published->queued_commit_seq = mapping.commit_seq;
    staged_size_ = 0;
    return true;
}

const PendingRevoke *CommandTable::next_durable_pending(uint64_t boot_epoch,
                                                        uint64_t durable_seq) const {
    if (boot_epoch == 0U) {
        return nullptr;
    }
    const PendingRevoke *first = nullptr;
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        const auto &control = pending_[index];
        if (control.accepted.principal_id != 0U && control.queued_boot_epoch == boot_epoch &&
            control.queued_commit_seq != 0U && control.queued_commit_seq <= durable_seq &&
            (first == nullptr || control.queued_commit_seq < first->queued_commit_seq ||
             (control.queued_commit_seq == first->queued_commit_seq &&
              control.accepted.target_grant_id < first->accepted.target_grant_id))) {
            first = &control;
        }
    }
    return first;
}

CommandTable::Lookup CommandTable::recovered_revoke(const PendingRevoke &control,
                                                    const CommandMapping *&out_mapping) const {
    if (!owns_pending(control)) {
        return Lookup::conflict;
    }
    const auto *mapping = mapped(control.accepted.principal_id, control.accepted.command_id);
    if (mapping == nullptr) {
        return Lookup::unmapped; // Floor cannot erase this earlier accepted control.
    }
    const auto bytes = canonical(*mapping);
    if (mapping->kind != AYNI_INTENT_GRANT_REVOKE || bytes.size != revoke_command_size ||
        std::memcmp(bytes.data, control.canonical.data(), revoke_command_size) != 0 ||
        !valid_authority_mapping(*mapping, bytes)) {
        return Lookup::conflict;
    }
    out_mapping = mapping;
    return Lookup::retried;
}

bool CommandTable::retire_pending(uint64_t principal_id, uint64_t command_id) {
    auto *control = pending_slot(principal_id, command_id);
    const CommandMapping *mapping = nullptr;
    if (control == nullptr || recovered_revoke(*control, mapping) != Lookup::retried) {
        return false;
    }
    // The table owns this mapping; only retention changes, never durable data.
    const_cast<CommandMapping *>(mapping)->retention = CommandRetention::normal;
    *control = PendingRevoke{};
    return true;
}

uint32_t CommandTable::pending_size() const {
    uint32_t size = 0;
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        size += pending_[index].accepted.principal_id != 0U ? 1U : 0U;
    }
    return size;
}

CommandTable::Lookup CommandTable::lookup_receipt(uint64_t principal_id, uint64_t command_id,
                                                  const PendingRevoke *&out_pending,
                                                  const CommandMapping *&out_mapping) const {
    out_pending = pending(principal_id, command_id);
    out_mapping = mapped(principal_id, command_id);
    if (out_pending != nullptr || out_mapping != nullptr) {
        return Lookup::retried;
    }
    return command_id <= floor(principal_id) ? Lookup::expired : Lookup::unmapped;
}

void CommandTable::abandon_queued_for_reload() {
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        pending_[index].queued_boot_epoch = 0;
        pending_[index].queued_commit_seq = 0;
    }
}

} // namespace ayni::kernel
