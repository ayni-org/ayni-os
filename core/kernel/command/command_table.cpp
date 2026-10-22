#include "kernel/command/command_table.h"

#include <cstring>
#include <new>

namespace ayni::kernel {

bool valid_authority_mapping(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    AuthorityCommandRecord command{};
    return mapping.principal_id != 0U && mapping.command_id != 0U &&
           mapping.kind == AYNI_INTENT_GRANT_CREATE && mapping.object.kind == AYNI_OBJECT_GRANT &&
           mapping.object.id != 0U && mapping.object.revision == 1U && bytes.size != 0U &&
           mapping.canonical_size == bytes.size && read_authority_command(bytes, command) &&
           (command.flags & ~AYNI_GRANT_ALLOW_DELEGATION) == 0U &&
           mapping.digest == authority_command_digest(bytes);
}

std::size_t CommandTable::storage_size(uint32_t rows, uint32_t capacity,
                                       uint32_t canonical_max_bytes) {
    static_assert(alignof(CommandMapping) <= storage_alignment, "mappings follow the rows");
    static_assert(sizeof(Row) % alignof(CommandMapping) == 0, "the mappings are aligned");
    const auto slots = static_cast<std::size_t>(rows) * capacity;
    return static_cast<std::size_t>(rows) * sizeof(Row) + slots * sizeof(CommandMapping) +
           (slots + 1U) * canonical_max_bytes;
}

CommandTable::CommandTable(void *storage, uint32_t rows, uint32_t capacity,
                           uint32_t canonical_max_bytes)
    : rows_(static_cast<Row *>(storage)),
      mappings_(reinterpret_cast<CommandMapping *>(static_cast<uint8_t *>(storage) +
                                                   static_cast<std::size_t>(rows) * sizeof(Row))),
      canonical_(
          reinterpret_cast<uint8_t *>(mappings_ + static_cast<std::size_t>(rows) * capacity)),
      scratch_(canonical_ + static_cast<std::size_t>(rows) * capacity * canonical_max_bytes),
      row_count_(rows), capacity_(capacity), canonical_max_bytes_(canonical_max_bytes) {
    for (uint32_t row = 0; row < rows; ++row) {
        new (rows_ + row) Row{};
    }
}

CommandTable::Lookup CommandTable::find(uint64_t principal_id, uint64_t command_id, uint64_t digest,
                                        const CommandMapping *&out_mapping) const {
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

CommandTable::ExactLookup CommandTable::find_exact(uint64_t principal_id, uint64_t command_id,
                                                   const CanonicalCommandView &input) {
    staged_size_ = 0;
    uint32_t needed = 0;
    auto status = measure_authority_command(input, needed);
    if (status != AYNI_OK || principal_id == 0U || command_id == 0U) {
        return {status == AYNI_OK ? AYNI_INVALID_ARGUMENT : status, Lookup::unmapped, nullptr};
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
    if (existing != nullptr &&
        (existing->kind != AYNI_INTENT_GRANT_CREATE || existing->canonical_size != needed)) {
        return {AYNI_OK, Lookup::conflict, nullptr};
    }
    if (needed > canonical_max_bytes_) {
        return {AYNI_CAPACITY_EXCEEDED, Lookup::unmapped, nullptr};
    }
    status = encode_authority_command(input, scratch_, canonical_max_bytes_, needed);
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
    staged_size_ = needed;
    staged_principal_ = principal_id;
    staged_command_ = command_id;
    staged_digest_ = digest;
    return {AYNI_OK, Lookup::unmapped, nullptr};
}

bool CommandTable::insert_exact(CommandMapping mapping) {
    if (staged_size_ == 0U || mapping.principal_id != staged_principal_ ||
        mapping.command_id != staged_command_ || mapping.kind != AYNI_INTENT_GRANT_CREATE ||
        !has_row(mapping.principal_id)) {
        return false;
    }
    mapping.digest = staged_digest_;
    mapping.canonical_size = staged_size_;
    if (!valid_authority_mapping(mapping, {scratch_, staged_size_})) {
        return false;
    }
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    add(*row, mapping, {scratch_, staged_size_});
    staged_size_ = 0;
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

void CommandTable::insert(const CommandMapping &mapping) {
    staged_size_ = 0;
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    // Ordinary insert never consumes an earlier exact candidate's scratch.
    CommandMapping ordinary = mapping;
    ordinary.canonical_size = 0;
    add(*row, ordinary, {});
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
    staged_size_ = 0;
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
    } else if (mapping.kind == AYNI_INTENT_GRANT_CREATE) {
        return Restore::corrupt;
    }
    if (!has_row(mapping.principal_id)) {
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
    add(*row, mapping, bytes);
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

void CommandTable::clear() {
    for (uint32_t row = 0; row < row_count_; ++row) {
        rows_[row] = Row{};
    }
    staged_size_ = 0;
    last_added_seq_ = 0;
    last_added_principal_ = 0;
    last_added_command_ = 0;
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

void CommandTable::add(Row &row, const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    CommandMapping *mappings = mappings_of(row);
    if (row.count == capacity_) {
        uint32_t smallest = 0;
        for (uint32_t index = 1; index < row.count; ++index) {
            if (mappings[index].command_id < mappings[smallest].command_id) {
                smallest = index;
            }
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
}

} // namespace ayni::kernel
