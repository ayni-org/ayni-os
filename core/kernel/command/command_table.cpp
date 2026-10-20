#include "kernel/command/command_table.h"

#include <new>

namespace ayni::kernel {

std::size_t CommandTable::storage_size(uint32_t rows, uint32_t capacity) {
    static_assert(alignof(CommandMapping) <= storage_alignment, "mappings follow the rows");
    static_assert(sizeof(Row) % alignof(CommandMapping) == 0, "the mappings are aligned");
    return static_cast<std::size_t>(rows) * sizeof(Row) +
           static_cast<std::size_t>(rows) * capacity * sizeof(CommandMapping);
}

CommandTable::CommandTable(void *storage, uint32_t rows, uint32_t capacity)
    : rows_(static_cast<Row *>(storage)),
      mappings_(reinterpret_cast<CommandMapping *>(static_cast<uint8_t *>(storage) +
                                                   static_cast<std::size_t>(rows) * sizeof(Row))),
      row_count_(rows), capacity_(capacity) {
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
            if (mappings[index].digest != digest) {
                return Lookup::conflict;
            }
            out_mapping = &mappings[index];
            return Lookup::retried;
        }
    }
    return command_id <= row->floor ? Lookup::expired : Lookup::unmapped;
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
    Row *row = row_of(mapping.principal_id);
    if (row == nullptr) {
        row = new_row(mapping.principal_id);
    }
    add(*row, mapping);
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
    const CommandMapping *existing = nullptr;
    if (mapping.principal_id == 0U || mapping.command_id == 0U || !has_row(mapping.principal_id)) {
        return false;
    }
    // Expired is fine: a boot with a smaller capacity gives way to more, so
    // its floor may be above the one the mapping was added under.
    const Lookup lookup = find(mapping.principal_id, mapping.command_id, mapping.digest, existing);
    if (lookup == Lookup::retried || lookup == Lookup::conflict) {
        return false;
    }
    insert(mapping);
    Row &row = *row_of(mapping.principal_id);
    if (floor > row.floor) {
        row.floor = floor;
    }
    return true;
}

void CommandTable::clear() {
    for (uint32_t row = 0; row < row_count_; ++row) {
        rows_[row] = Row{};
    }
    last_added_seq_ = 0;
}

CommandTable::Row *CommandTable::row_of(uint64_t principal_id) const {
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (rows_[row].principal_id == principal_id) {
            return &rows_[row];
        }
    }
    return nullptr;
}

CommandTable::Row *CommandTable::new_row(uint64_t principal_id) {
    for (uint32_t row = 0; row < row_count_; ++row) {
        if (rows_[row].principal_id == 0U) {
            rows_[row] = Row{principal_id, 0, 0};
            return &rows_[row];
        }
    }
    return nullptr;
}

CommandMapping *CommandTable::mappings_of(const Row &row) const {
    return mappings_ + static_cast<std::size_t>(&row - rows_) * capacity_;
}

void CommandTable::add(Row &row, const CommandMapping &mapping) {
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
        mappings[smallest] = mappings[row.count - 1U];
        --row.count;
    }
    mappings[row.count] = mapping;
    ++row.count;
    last_added_seq_ = mapping.commit_seq;
    last_added_principal_ = mapping.principal_id;
    last_added_command_ = mapping.command_id;
}

} // namespace ayni::kernel
