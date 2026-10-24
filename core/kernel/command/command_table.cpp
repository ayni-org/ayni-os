#include "kernel/command/command_table.h"

#include "authority/grant/grant_table.h"

#include <cstring>
#include <new>

namespace ayni::kernel {

bool valid_authority_mapping(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    if (mapping.principal_id == 0U || mapping.command_id == 0U ||
        mapping.object.kind !=
            (mapping.kind == AYNI_INTENT_GOAL_APPROVE ? AYNI_OBJECT_GOAL : AYNI_OBJECT_GRANT) ||
        mapping.object.id == 0U || mapping.canonical_size != bytes.size || bytes.size == 0U) {
        return false;
    }
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE) {
        AuthorityCommandRecord command{};
        if (mapping.object.revision != authority::grant_active_revision ||
            mapping.effect != AYNI_INTENT_EFFECT_CHANGED ||
            !read_authority_command(bytes, command) ||
            !authority::valid_persisted_grant_flags(command.flags)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        RevokeCommandRecord command{};
        if (!read_revoke_command(bytes, command) || mapping.object.origin_device_id != 0U ||
            mapping.object.id != command.target_grant_id ||
            (command.expected_revision != authority::grant_active_revision &&
             command.expected_revision != authority::grant_terminal_revision)) {
            return false;
        }
        // First acceptance can only see ACTIVE/revision1 or terminal/revision2.
        // The raw request codec still accepts any nonzero CAS for API conflicts.
        // The original CAS was checked when accepting the control. Reload
        // may restore an earlier ACTIVE fact after an uncommitted cleanup,
        // so a later CHANGED result need not follow that original revision.
        const bool changed = mapping.effect == AYNI_INTENT_EFFECT_CHANGED &&
                             mapping.object.revision == authority::grant_terminal_revision;
        const bool no_change = mapping.effect == AYNI_INTENT_EFFECT_NO_CHANGE &&
                               mapping.object.revision == authority::grant_terminal_revision;
        const bool gone =
            mapping.effect == AYNI_INTENT_EFFECT_TARGET_GONE && mapping.object.revision == 0U;
        if (!changed && !no_change && !gone) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_GOAL_APPROVE) {
        ApprovalCommand command{};
        if (!read_approval_command(bytes, command) || mapping.object.origin_device_id != 0U ||
            command.expected_revision == UINT32_MAX ||
            mapping.object.id != command.approval.goal_id ||
            mapping.object.revision != command.expected_revision + 1U ||
            mapping.effect != AYNI_INTENT_EFFECT_CHANGED) {
            return false;
        }
    } else {
        return false;
    }
    return mapping.digest == authority_command_digest(bytes);
}

std::size_t CommandTable::storage_size(uint32_t rows, uint32_t capacity,
                                       uint32_t canonical_max_bytes, uint32_t pending_capacity) {
    static_assert(alignof(CommandMapping) <= storage_alignment, "mappings follow the rows");
    static_assert(sizeof(Row) % alignof(CommandMapping) == 0, "the mappings are aligned");
    const auto slots = static_cast<std::size_t>(rows) * capacity;
    static_assert(alignof(PendingRevoke) <= storage_alignment, "pending controls follow mappings");
    static_assert(sizeof(CommandMapping) % alignof(PendingRevoke) == 0,
                  "the pending metadata is aligned before byte strides");
    return static_cast<std::size_t>(rows) * sizeof(Row) + slots * sizeof(CommandMapping) +
           static_cast<std::size_t>(pending_capacity) * sizeof(PendingRevoke) +
           (slots + 1U) * canonical_max_bytes;
}

CommandTable::CommandTable(void *storage, uint32_t rows, uint32_t capacity,
                           uint32_t canonical_max_bytes, uint32_t pending_capacity)
    : rows_(static_cast<Row *>(storage)),
      mappings_(reinterpret_cast<CommandMapping *>(static_cast<uint8_t *>(storage) +
                                                   static_cast<std::size_t>(rows) * sizeof(Row))),
      pending_(
          reinterpret_cast<PendingRevoke *>(mappings_ + static_cast<std::size_t>(rows) * capacity)),
      canonical_(reinterpret_cast<uint8_t *>(pending_ + pending_capacity)),
      scratch_(canonical_ + static_cast<std::size_t>(rows) * capacity * canonical_max_bytes),
      row_count_(rows), capacity_(capacity), canonical_max_bytes_(canonical_max_bytes),
      pending_capacity_(pending_capacity) {
    for (uint32_t row = 0; row < rows; ++row) {
        new (rows_ + row) Row{};
    }
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        new (pending_ + index) PendingRevoke{};
    }
}

CommandTable::Lookup CommandTable::find(uint64_t principal_id, uint64_t command_id, uint64_t digest,
                                        const CommandMapping *&out_mapping) const {
    if (pending(principal_id, command_id) != nullptr) {
        return Lookup::conflict; // This lookup is ordinary, pending identities are exact revokes.
    }
    const Row *row = row_of(principal_id);
    if (row == nullptr) {
        return Lookup::unmapped;
    }
    const CommandMapping *mappings = mappings_of(*row);
    for (uint32_t index = 0; index < row->count; ++index) {
        if (mappings[index].command_id == command_id) {
            // A legacy digest-only lookup cannot establish exact identity,
            // even if it collides with an authorization command's digest.
            if (mappings[index].canonical_size != 0U || mappings[index].digest != digest) {
                return Lookup::conflict;
            }
            out_mapping = &mappings[index];
            return Lookup::retried;
        }
    }
    return command_id <= row->floor ? Lookup::expired : Lookup::unmapped;
}

// Shape is independent of retention. Lookup/floor precedes configured byte
// capacity, so a stale or conflicting command keeps its original classification.
template <typename Input>
CommandTable::ExactLookup CommandTable::find_candidate(
    uint64_t principal_id, uint64_t command_id, ayni_intent_kind_t kind, const Input &input,
    ayni_status_t (*measure)(const Input &, uint32_t &),
    ayni_status_t (*encode)(const Input &, uint8_t *, uint32_t, uint32_t &)) {
    staged_.reset();
    uint32_t needed = 0;
    auto status = measure(input, needed);
    if (status != AYNI_OK || principal_id == 0U || command_id == 0U) {
        return {status == AYNI_OK ? AYNI_INVALID_ARGUMENT : status, Lookup::unmapped, nullptr};
    }
    if (pending(principal_id, command_id) != nullptr) {
        return {AYNI_OK, Lookup::conflict, nullptr};
    }
    const Row *row = row_of(principal_id);
    const CommandMapping *existing = nullptr;
    if (row != nullptr) {
        const auto *mappings = mappings_of(*row);
        for (uint32_t index = 0; index < row->count; ++index) {
            if (mappings[index].command_id == command_id) {
                existing = &mappings[index];
                break;
            }
        }
        if (existing == nullptr && command_id <= row->floor) {
            return {AYNI_OK, Lookup::expired, nullptr};
        }
    }
    if (existing != nullptr && (existing->kind != kind || existing->canonical_size != needed)) {
        return {AYNI_OK, Lookup::conflict, nullptr};
    }
    if (needed > canonical_max_bytes_) {
        return {AYNI_CAPACITY_EXCEEDED, Lookup::unmapped, nullptr};
    }
    status = encode(input, scratch_, canonical_max_bytes_, needed);
    if (status != AYNI_OK) {
        return {status, Lookup::unmapped, nullptr};
    }
    const uint64_t digest = authority_command_digest({scratch_, needed});
    if (existing != nullptr) {
        if (existing->digest != digest ||
            std::memcmp(canonical_of(*existing), scratch_, needed) != 0) {
            return {AYNI_OK, Lookup::conflict, nullptr};
        }
        return {AYNI_OK, Lookup::retried, existing};
    }
    staged_ = StagedCommand{principal_id, command_id, digest, needed, kind};
    return {AYNI_OK, Lookup::unmapped, nullptr};
}

CommandTable::ExactLookup CommandTable::find_exact(uint64_t principal_id, uint64_t command_id,
                                                   const CanonicalCommandView &input) {
    return find_candidate(principal_id, command_id, AYNI_INTENT_GRANT_CREATE, input,
                          measure_authority_command, encode_authority_command);
}

CommandTable::ExactLookup CommandTable::find_exact(uint64_t principal_id, uint64_t command_id,
                                                   const ApprovalCommand &input) {
    return find_candidate(principal_id, command_id, AYNI_INTENT_GOAL_APPROVE, input,
                          measure_approval_command, encode_approval_command);
}

bool CommandTable::insert_exact(CommandMapping mapping) {
    if (!staged_ || mapping.principal_id != staged_->principal_id ||
        mapping.command_id != staged_->command_id || mapping.kind != staged_->kind ||
        !can_insert(mapping.principal_id)) {
        return false;
    }
    mapping.digest = staged_->digest;
    mapping.canonical_size = staged_->size;
    mapping.retention = CommandRetention::normal;
    if (!valid_authority_mapping(mapping, {scratch_, staged_->size})) {
        return false;
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    if (!add(*row, mapping, {scratch_, staged_->size})) {
        return false;
    }
    staged_.reset();
    return true;
}

ayni_bytes_v1 CommandTable::canonical(const CommandMapping &mapping) const {
    return {mapping.canonical_size == 0U ? nullptr : canonical_of(mapping), mapping.canonical_size};
}

bool CommandTable::has_row(uint64_t principal_id) const {
    if (row_of(principal_id) != nullptr) {
        return true;
    }
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (rows_[row].principal_id == 0U) {
            return true;
        }
    }
    return false;
}

bool CommandTable::insert(const CommandMapping &mapping) {
    staged_.reset();
    if (!can_insert(mapping.principal_id) || mapping.kind < AYNI_INTENT_GOAL_CREATE ||
        mapping.kind > AYNI_INTENT_INSTANCE_STOP ||
        pending(mapping.principal_id, mapping.command_id) != nullptr) {
        return false;
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    // Ordinary insert never consumes an earlier exact candidate's scratch.
    CommandMapping ordinary = mapping;
    ordinary.canonical_size = 0;
    ordinary.effect = AYNI_INTENT_EFFECT_CHANGED;
    ordinary.retention = CommandRetention::normal;
    return add(*row, ordinary, {});
}

const CommandMapping *CommandTable::mapped_in(uint64_t commit_seq) const {
    if (commit_seq == 0U || commit_seq != last_added_seq_) {
        return nullptr;
    }
    const Row *row = row_of(last_added_principal_);
    const CommandMapping *mappings = mappings_of(*row);
    for (uint32_t index = 0; index < row->count; ++index) {
        if (mappings[index].command_id == last_added_command_) {
            return &mappings[index];
        }
    }
    return nullptr;
}

uint64_t CommandTable::floor(uint64_t principal_id) const {
    const Row *row = row_of(principal_id);
    return row != nullptr ? row->floor : 0U;
}

bool CommandTable::restore(const CommandMapping &mapping, uint64_t floor) {
    return restore_checked(mapping, floor) == Restore::restored;
}

CommandTable::Restore CommandTable::restore_checked(const CommandMapping &mapping, uint64_t floor,
                                                    ayni_bytes_v1 bytes) {
    staged_.reset();
    if (mapping.principal_id == 0U || mapping.command_id == 0U ||
        mapping.canonical_size != bytes.size) {
        return Restore::corrupt;
    }
    if (mapping.canonical_size != 0U) {
        if (!valid_authority_mapping(mapping, bytes)) {
            return Restore::corrupt;
        }
        if (bytes.size > canonical_max_bytes_) {
            return Restore::no_room;
        }
    } else if (mapping.kind == AYNI_INTENT_GRANT_CREATE ||
               mapping.kind == AYNI_INTENT_GRANT_REVOKE ||
               mapping.kind == AYNI_INTENT_GOAL_APPROVE) {
        return Restore::corrupt;
    }
    if (!can_insert(mapping.principal_id)) {
        return Restore::no_room;
    }
    const Row *existing_row = row_of(mapping.principal_id);
    if (existing_row != nullptr) {
        const auto *mappings = mappings_of(*existing_row);
        for (uint32_t index = 0; index < existing_row->count; ++index) {
            if (mappings[index].command_id == mapping.command_id) {
                return Restore::corrupt;
            }
        }
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    // A smaller window may already have raised its floor above this command.
    // Keep the replayed mapping, and never reduce either source of the floor.
    CommandMapping restored = mapping;
    restored.retention = CommandRetention::normal;
    if (!add(*row, restored, bytes)) {
        return Restore::no_room;
    }
    if (floor > row->floor) {
        row->floor = floor;
    }
    return Restore::restored;
}

uint32_t CommandTable::size() const {
    uint32_t count = 0;
    for (uint32_t row = 0; row < row_count_; ++row) {
        count += rows_[row].count;
    }
    return count;
}

const CommandMapping &CommandTable::at(uint32_t index) const {
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (index < rows_[row].count) {
            return mappings_of(rows_[row])[index];
        }
        index -= rows_[row].count;
    }
    // Only called with index < size().
    return mappings_[0];
}

void CommandTable::clear_restored() {
    abandon_queued_for_reload();
    for (uint32_t row = 0; row < row_count_; ++row) {
        rows_[row] = Row{};
    }
    staged_.reset();
    last_added_seq_ = 0;
    last_added_principal_ = 0;
    last_added_command_ = 0;
}

void CommandTable::clear() {
    clear_restored();
    for (uint32_t index = 0; index < pending_capacity_; ++index) {
        pending_[index] = PendingRevoke{};
    }
}

CommandTable::Row *CommandTable::row_of(uint64_t principal_id) const {
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (rows_[row].principal_id == principal_id) {
            return rows_ + row;
        }
    }
    return nullptr;
}

CommandTable::Row *CommandTable::new_row(uint64_t principal_id) {
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (rows_[row].principal_id == 0U) {
            rows_[row] = Row{principal_id, 0, 0};
            return rows_ + row;
        }
    }
    return nullptr;
}

CommandMapping *CommandTable::mappings_of(const Row &row) const {
    return mappings_ + static_cast<std::size_t>(&row - rows_) * capacity_;
}

uint8_t *CommandTable::canonical_of(const CommandMapping &mapping) const {
    return canonical_ + static_cast<std::size_t>(&mapping - mappings_) * canonical_max_bytes_;
}

bool CommandTable::add(Row &row, const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    CommandMapping *mappings = mappings_of(row);
    if (row.count == capacity_) {
        uint32_t smallest = row.count;
        for (uint32_t index = 0; index < row.count; ++index) {
            if (mappings[index].retention == CommandRetention::normal &&
                (smallest == row.count ||
                 mappings[index].command_id < mappings[smallest].command_id)) {
                smallest = index;
            }
        }
        if (smallest == row.count) {
            return false; // All accepted revokes still need a durable result.
        }
        if (mappings[smallest].command_id > row.floor) {
            row.floor = mappings[smallest].command_id;
        }
        const uint32_t last = row.count - 1U;
        if (smallest != last) {
            if (mappings[last].canonical_size != 0U) {
                std::memmove(canonical_of(mappings[smallest]), canonical_of(mappings[last]),
                             mappings[last].canonical_size);
            }
            mappings[smallest] = mappings[last];
        }
        --row.count;
    }
    CommandMapping &added = mappings[row.count];
    if (bytes.size != 0U) {
        std::memcpy(canonical_of(added), bytes.data, bytes.size);
    }
    added = mapping;
    ++row.count;
    last_added_seq_ = mapping.commit_seq;
    last_added_principal_ = mapping.principal_id;
    last_added_command_ = mapping.command_id;
    return true;
}

} // namespace ayni::kernel
