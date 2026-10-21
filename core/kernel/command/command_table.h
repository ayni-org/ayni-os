// Management command idempotency (C-WIRE-003, C-WIRE-004, ADR-0026). An
// intent with a nonzero command_id is a command of its session's principal,
// and the kernel maps each command it accepted to the result it gave, in the
// transition that applied it, so the mapping is durable with that result. A
// retry of the same command, same content, gets that result again without a
// new transition; the same command id with other content is a conflict. A
// refused command maps to nothing, so a retry is judged anew.
//
// Each principal keeps at most capacity mappings. When a new one does not
// fit, the mapping with the smallest command id gives way and raises the
// principal's floor to that id: a command id at or below the floor that is
// not mapped may have been accepted once, so it is refused as expired rather
// than applied again. Callers that number their commands upwards lose
// nothing but the oldest retries.

#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

struct CommandMapping {
    uint64_t principal_id;
    uint64_t command_id; // nonzero
    uint64_t digest;     // command_digest() of what was accepted
    uint64_t commit_seq; // of the transition that applied it
    ayni_intent_kind_t kind;
    ayni_object_ref_v1 object; // the result's
};

class CommandTable {
  public:
    // What find() made of a command.
    enum class Lookup {
        unmapped, // not accepted before, as far as the table knows
        retried,  // accepted before with the same digest: out_mapping
        conflict, // accepted before with another digest
        expired,  // not mapped, at or below the principal's floor
    };
    enum class Restore { restored, corrupt, no_room };

    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    // rows principals with capacity mappings each.
    static std::size_t storage_size(uint32_t rows, uint32_t capacity);

    // storage holds storage_size(rows, capacity) bytes aligned to
    // storage_alignment and outlives the table; capacity is at least 1.
    CommandTable(void *storage, uint32_t rows, uint32_t capacity);

    CommandTable(const CommandTable &) = delete;
    CommandTable &operator=(const CommandTable &) = delete;

    Lookup find(uint64_t principal_id, uint64_t command_id, uint64_t digest,
                const CommandMapping *&out_mapping) const;
    // Whether insert() has a row for the principal.
    bool has_row(uint64_t principal_id) const;
    // Maps a command that find() returned unmapped for, giving way as above.
    // Only when has_row(mapping.principal_id).
    void insert(const CommandMapping &mapping);
    // The mapping insert() or restore() added with commit_seq, if any.
    const CommandMapping *mapped_in(uint64_t commit_seq) const;
    // The principal's floor; 0 before anything gave way.
    uint64_t floor(uint64_t principal_id) const;

    // Replays a COMMAND_MAPPING record: the mapping, which may give way as
    // insert() does, and the floor it left. Returns false, changing
    // nothing, for a mapping the table already has or with no row left.
    bool restore(const CommandMapping &mapping, uint64_t floor);
    Restore restore_checked(const CommandMapping &mapping, uint64_t floor);
    uint32_t size() const;
    const CommandMapping &at(uint32_t index) const;
    // Empties the table, as a recovery that failed leaves it.
    void clear();

  private:
    struct Row {
        uint64_t principal_id; // 0: unused
        uint64_t floor;
        uint32_t count;
    };

    Row *row_of(uint64_t principal_id) const;
    Row *new_row(uint64_t principal_id);
    CommandMapping *mappings_of(const Row &row) const;
    void add(Row &row, const CommandMapping &mapping);

    Row *rows_;
    CommandMapping *mappings_;
    uint32_t row_count_;
    uint32_t capacity_;
    // The latest mapping added and the commit_seq it was added with.
    uint64_t last_added_seq_ = 0;
    uint64_t last_added_principal_ = 0;
    uint64_t last_added_command_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H
