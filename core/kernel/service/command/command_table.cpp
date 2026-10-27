#include "kernel/service/command/command_table.h"

#include "authority/grant/grant_table.h"

#include <cstring>
#include <new>

namespace ayni::kernel {
namespace {

// Exact target commands publish one CAS increment and never retarget the object.
bool matches_target_revision(const CommandMapping &mapping, uint64_t id, uint32_t before) {
    return before != UINT32_MAX && mapping.object.origin_device_id == 0U &&
           mapping.object.id == id && mapping.object.revision == before + 1U &&
           mapping.effect == AYNI_INTENT_EFFECT_CHANGED;
}

// The complete typed request and CAS result share one intrinsic command proof.
bool matches_pause_group_result(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    work::PauseGroupChange command{};
    return read_pause_group_command(bytes, command) &&
           matches_target_revision(mapping, mapping.object.id, command.expected_revision);
}

// Revocation's changed/no-change/gone receipt is independent of target retention.
bool matches_revoke_result(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
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
    return true;
}

// Reauthorization's exact request binds both CAS values to the saved result;
// no current Goal, Session or catalog lookup belongs to command identity.
bool matches_reauthorization_command(const CommandMapping &mapping, ayni_bytes_v1 bytes,
                                     const WorkAuthorityResult &result) {
    ReauthorizationCommand command{};
    return mapping.result_size != 0U && read_reauthorization_command(bytes, command) &&
           command.expected_revision != UINT32_MAX && mapping.object.origin_device_id == 0U &&
           mapping.object.id == command.reauthorization.goal_id &&
           mapping.object.revision == command.expected_revision + 1U &&
           result.binding.parent.goal_id == command.reauthorization.original.parent_goal_id &&
           result.binding.input.incarnation != 0U &&
           command.reauthorization.expected_authority_commit_seq < result.binding.established_seq &&
           mapping.effect == AYNI_INTENT_EFFECT_CHANGED;
}
// Context OPEN has the initial privacy revision; publication and ACK preserve it, FORGET advances.
bool matches_context_result(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    ContextCommand command{};
    if (!read_context_command(bytes, command) || command.kind != mapping.kind ||
        mapping.object.origin_device_id != 0 || mapping.effect != AYNI_INTENT_EFFECT_CHANGED ||
        (command.context_id != 0 && mapping.object.id != command.context_id)) {
        return false;
    }
    const auto revision = command.kind == AYNI_INTENT_CONTEXT_OPEN ? 1U
                          : command.kind == AYNI_INTENT_CONTEXT_FORGET
                              ? command.expected_revision + 1U
                              : command.expected_revision;
    if (revision == 0 || mapping.object.revision != revision) {
        return false;
    }
    return true;
}
// Canonical Grant creation keeps the immutable active revision and admitted delegation flags.
bool matches_grant_result(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    AuthorityCommandRecord command{};
    if (mapping.object.revision != authority::grant_active_revision ||
        mapping.effect != AYNI_INTENT_EFFECT_CHANGED || !read_authority_command(bytes, command) ||
        !authority::valid_persisted_grant_flags(command.flags)) {
        return false;
    }
    return true;
}
} // namespace

bool valid_expired_mapping(const CommandMapping &m) {
    return implements_intent(m.kind) && m.privacy_expired && m.principal_id != 0 &&
           m.command_id != 0 && m.digest == 0 && m.canonical_size == 0 && m.result_size == 0 &&
           m.sources.count == 0 && m.object.origin_device_id == 0 && m.object.id != 0 &&
           m.object.revision != 0 && m.object.kind == intent_traits(m.kind).object &&
           (is_memory_command(m.kind) || is_work_authority_command(m.kind) ||
            (is_ordinary_command(m.kind) && m.object.kind == AYNI_OBJECT_GOAL)) &&
           m.effect == AYNI_INTENT_EFFECT_CHANGED && m.retention == CommandRetention::normal;
}

void CommandTable::discard_candidate() {
    std::memset(scratch_, 0, canonical_max_bytes_);
    staged_.reset();
}

// Privacy is independent of the first result: keep its identity/sequence but destroy all copies
// of exact private input, including unused slot tails and the ephemeral encoding candidate.
bool CommandTable::expire_content(uint64_t commit_seq) {
    for (uint32_t row = 0; row < row_count_; ++row) {
        auto *mappings = mappings_of(rows_[row]);
        for (uint32_t index = 0; index < rows_[row].count; ++index) {
            auto &mapping = mappings[index];
            if (mapping.commit_seq != commit_seq) {
                continue;
            }
            CommandMapping erased = mapping;
            erased.privacy_expired = true;
            erased.digest = 0;
            erased.canonical_size = 0;
            erased.result_size = 0;
            erased.sources = {};
            if (mapping.privacy_expired || !valid_expired_mapping(erased)) {
                return false;
            }
            std::memset(canonical_of(mapping), 0, canonical_max_bytes_);
            discard_candidate();
            mapping = erased;
            return true;
        }
    }
    return false;
}

// Validate the common envelope, then delegate each exact request to its domain codec.
// Dispatch complete scalar/target command proofs; grant/work authority proofs stay above.
bool matches_management_target(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    if (is_context_command(mapping.kind)) {
        if (!matches_context_result(mapping, bytes)) {
            return false;
        }
    } else if (is_memory_command(mapping.kind)) {
        MemoryCommand command{};
        if (!read_memory_command(bytes, command) || command.kind != mapping.kind ||
            !matches_target_revision(mapping,
                                     command.target_id == 0 ? mapping.object.id : command.target_id,
                                     command.expected_revision)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_PAUSE_GROUP_SET) {
        if (!matches_pause_group_result(mapping, bytes)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
        authority::SceneChange command{};
        if (!read_scene_command(bytes, command) || mapping.object.origin_device_id != 0 ||
            (mapping.effect == AYNI_INTENT_EFFECT_CHANGED
                 ? command.expected_revision == UINT32_MAX ||
                       mapping.object.revision != command.expected_revision + 1
                 : mapping.effect != AYNI_INTENT_EFFECT_NO_CHANGE ||
                       command.expected_revision == 0 ||
                       mapping.object.revision != command.expected_revision)) {
            return false;
        }
    } else if (is_target_command(mapping.kind)) {
        TargetCommand command{};
        if (!read_target_command(bytes, command) || command.kind != mapping.kind ||
            !matches_target_revision(mapping, command.target_id, command.expected_revision)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT) {
        TaskLimitCommand command{};
        if (!read_task_limit_command(bytes, command) ||
            !matches_target_revision(mapping, command.task_id, command.expected_revision)) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

bool valid_exact_mapping(const CommandMapping &mapping, ayni_bytes_v1 bytes,
                         ayni_bytes_v1 result_bytes) {
    if (mapping.privacy_expired || !is_exact_command(mapping.kind) || mapping.principal_id == 0U ||
        mapping.command_id == 0U || mapping.object.kind != intent_traits(mapping.kind).object ||
        mapping.object.id == 0U || mapping.canonical_size != bytes.size || bytes.size == 0U ||
        mapping.result_size != result_bytes.size ||
        (is_work_authority_command(mapping.kind) && result_bytes.size == 0U)) {
        return false;
    }
    WorkAuthorityResult result{};
    if (result_bytes.size != 0U &&
        (!is_work_authority_command(mapping.kind) ||
         !read_work_authority_result(result_bytes, mapping.object.id, result) ||
         (mapping.commit_seq != 0U && result.binding.established_seq != mapping.commit_seq))) {
        return false;
    }
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE) {
        if (!matches_grant_result(mapping, bytes)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        if (!matches_revoke_result(mapping, bytes)) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_GOAL_APPROVE) {
        ApprovalCommand command{};
        if (!read_approval_command(bytes, command) ||
            !matches_target_revision(mapping, command.approval.goal_id,
                                     command.expected_revision) ||
            (result_bytes.size != 0U &&
             (result.binding.parent.goal_id != command.approval.original.parent_goal_id ||
              result.binding.input.incarnation != 0U))) {
            return false;
        }
    } else if (mapping.kind == AYNI_INTENT_GOAL_REAUTHORIZE) {
        if (!matches_reauthorization_command(mapping, bytes, result)) {
            return false;
        }
    } else if (!matches_management_target(mapping, bytes)) {
        return false;
    }
    return mapping.digest == authority_command_digest(bytes);
}

std::size_t CommandTable::storage_size(const Limits &limits) {
    static_assert(alignof(CommandMapping) <= storage_alignment, "mappings follow the rows");
    static_assert(sizeof(Row) % alignof(CommandMapping) == 0, "the mappings are aligned");
    const auto slots = static_cast<std::size_t>(limits.rows) * limits.capacity;
    static_assert(alignof(PendingRevoke) <= storage_alignment, "pending controls follow mappings");
    static_assert(sizeof(CommandMapping) % alignof(PendingRevoke) == 0,
                  "the pending metadata is aligned before byte strides");
    return static_cast<std::size_t>(limits.rows) * sizeof(Row) + slots * sizeof(CommandMapping) +
           static_cast<std::size_t>(limits.pending_capacity) * sizeof(PendingRevoke) +
           (slots + 1U) * limits.canonical_max_bytes;
}

CommandTable::CommandTable(void *storage, const Limits &limits)
    : rows_(static_cast<Row *>(storage)),
      mappings_(reinterpret_cast<CommandMapping *>(
          static_cast<uint8_t *>(storage) + static_cast<std::size_t>(limits.rows) * sizeof(Row))),
      pending_(reinterpret_cast<PendingRevoke *>(mappings_ + static_cast<std::size_t>(limits.rows) *
                                                                 limits.capacity)),
      canonical_(reinterpret_cast<uint8_t *>(pending_ + limits.pending_capacity)),
      scratch_(canonical_ + static_cast<std::size_t>(limits.rows) * limits.capacity *
                                limits.canonical_max_bytes),
      row_count_(limits.rows), capacity_(limits.capacity),
      canonical_max_bytes_(limits.canonical_max_bytes), pending_capacity_(limits.pending_capacity) {
    for (uint32_t row = 0; row < row_count_; ++row) {
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
            if (mappings[index].privacy_expired) {
                return Lookup::expired;
            }
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
CommandTable::ExactProbe CommandTable::probe_exact(uint64_t principal_id, uint64_t command_id,
                                                   ayni_intent_kind_t kind, ayni_status_t measured,
                                                   uint32_t size) {
    discard_candidate();
    if (measured != AYNI_OK || principal_id == 0U || command_id == 0U) {
        return {{measured == AYNI_OK ? AYNI_INVALID_ARGUMENT : measured, Lookup::unmapped, nullptr},
                false,
                nullptr};
    }
    if (pending(principal_id, command_id) != nullptr) {
        return {{AYNI_OK, Lookup::conflict, nullptr}, false, nullptr};
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
            return {{AYNI_OK, Lookup::expired, nullptr}, false, nullptr};
        }
    }
    if (existing != nullptr && existing->privacy_expired) {
        return {{AYNI_OK, Lookup::expired, nullptr}, false, nullptr};
    }
    if (existing != nullptr && (existing->kind != kind || existing->canonical_size != size)) {
        return {{AYNI_OK, Lookup::conflict, nullptr}, false, nullptr};
    }
    if (size > canonical_max_bytes_) {
        return {{AYNI_CAPACITY_EXCEEDED, Lookup::unmapped, nullptr}, false, nullptr};
    }
    return {{AYNI_OK, Lookup::unmapped, nullptr}, true, existing};
}

CommandTable::ExactLookup CommandTable::stage_exact(uint64_t principal_id, uint64_t command_id,
                                                    ayni_intent_kind_t kind,
                                                    const CommandMapping *existing,
                                                    ayni_status_t encoded, uint32_t size) {
    if (encoded != AYNI_OK) {
        discard_candidate();
        return {encoded, Lookup::unmapped, nullptr};
    }
    const uint64_t digest = authority_command_digest({scratch_, size});
    if (existing != nullptr) {
        const bool same =
            existing->digest == digest && std::memcmp(canonical_of(*existing), scratch_, size) == 0;
        discard_candidate();
        if (!same) {
            return {AYNI_OK, Lookup::conflict, nullptr};
        }
        return {AYNI_OK, Lookup::retried, existing};
    }
    staged_ = StagedCommand{principal_id, command_id, digest, size, kind};
    return {AYNI_OK, Lookup::unmapped, nullptr};
}

bool CommandTable::insert_exact(CommandMapping mapping, ayni_bytes_v1 result) {
    if (!staged_ || mapping.principal_id != staged_->principal_id ||
        mapping.command_id != staged_->command_id || mapping.kind != staged_->kind ||
        !can_insert(mapping.principal_id)) {
        return false;
    }
    mapping.digest = staged_->digest;
    mapping.canonical_size = staged_->size;
    mapping.result_size = result.size;
    mapping.retention = CommandRetention::normal;
    if (result.size > canonical_max_bytes_ - staged_->size ||
        !valid_exact_mapping(mapping, {scratch_, staged_->size}, result)) {
        return false;
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    if (!add(*row, mapping, {scratch_, staged_->size}, result)) {
        return false;
    }
    discard_candidate();
    return true;
}

ayni_bytes_v1 CommandTable::canonical(const CommandMapping &mapping) const {
    return {mapping.canonical_size == 0U ? nullptr : canonical_of(mapping), mapping.canonical_size};
}

ayni_bytes_v1 CommandTable::authorization_result(const CommandMapping &mapping) const {
    return {mapping.result_size == 0U ? nullptr : canonical_of(mapping) + mapping.canonical_size,
            mapping.result_size};
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
    if (!can_insert(mapping.principal_id) || !is_ordinary_command(mapping.kind) ||
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
    ordinary.result_size = 0;
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
                                                    ayni_bytes_v1 bytes, ayni_bytes_v1 result) {
    staged_.reset();
    if (mapping.principal_id == 0U || mapping.command_id == 0U ||
        mapping.canonical_size != bytes.size || mapping.result_size != result.size ||
        (mapping.canonical_size == 0U && result.size != 0U)) {
        return Restore::corrupt;
    }
    if (mapping.privacy_expired) {
        if (!valid_expired_mapping(mapping)) {
            return Restore::corrupt;
        }
    } else if (mapping.canonical_size != 0U) {
        if (!valid_exact_mapping(mapping, bytes, result)) {
            return Restore::corrupt;
        }
        if (bytes.size > canonical_max_bytes_ || result.size > canonical_max_bytes_ - bytes.size) {
            return Restore::no_room;
        }
    } else if (is_exact_command(mapping.kind)) {
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
    if (!add(*row, restored, bytes, result)) {
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

// One eviction selector serves admission's Grant unlink fence and the table's actual replacement.
const CommandMapping *CommandTable::eviction_candidate(uint64_t principal_id) const {
    const auto *row = row_of(principal_id);
    if (row == nullptr || row->count < capacity_) {
        return nullptr;
    }
    const auto *mappings = mappings_of(*row);
    const CommandMapping *candidate = nullptr;
    for (uint32_t index = 0; index < row->count; ++index) {
        if (mappings[index].retention == CommandRetention::normal &&
            (candidate == nullptr || mappings[index].command_id < candidate->command_id)) {
            candidate = mappings + index;
        }
    }
    return candidate;
}

bool CommandTable::add(Row &row, const CommandMapping &mapping, ayni_bytes_v1 bytes,
                       ayni_bytes_v1 result) {
    CommandMapping *mappings = mappings_of(row);
    if (row.count == capacity_) {
        const auto *evicted = eviction_candidate(row.principal_id);
        if (evicted == nullptr) {
            return false; // All accepted revokes still need a durable result.
        }
        const auto smallest = static_cast<uint32_t>(evicted - mappings);
        if (mappings[smallest].command_id > row.floor) {
            row.floor = mappings[smallest].command_id;
        }
        const uint32_t last = row.count - 1U;
        if (smallest != last) {
            std::memset(canonical_of(mappings[smallest]), 0, canonical_max_bytes_);
            if (mappings[last].canonical_size != 0U) {
                std::memmove(canonical_of(mappings[smallest]), canonical_of(mappings[last]),
                             mappings[last].canonical_size + mappings[last].result_size);
            }
            mappings[smallest] = mappings[last];
        }
        --row.count;
    }
    CommandMapping &added = mappings[row.count];
    // A shorter replacement must not retain the evicted request in its unused byte tail.
    std::memset(canonical_of(added), 0, canonical_max_bytes_);
    if (bytes.size != 0U) {
        std::memcpy(canonical_of(added), bytes.data, bytes.size);
    }
    if (result.size != 0U) {
        std::memcpy(canonical_of(added) + bytes.size, result.data, result.size);
    }
    added = mapping;
    ++row.count;
    last_added_seq_ = mapping.commit_seq;
    last_added_principal_ = mapping.principal_id;
    last_added_command_ = mapping.command_id;
    return true;
}

} // namespace ayni::kernel
