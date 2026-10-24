// Management command idempotency (C-WIRE-003, C-WIRE-004, ADR-0026). An
// intent with a nonzero command_id is a command of its session's principal,
// and the kernel maps each command it accepted to the result it gave, in the
// transition that applied it, so the mapping is durable with that result. A
// retry of the same command, same content, gets that result again without a
// new transition; the same command id with other content is a conflict. A
// refused command maps to nothing, so a retry is judged anew.
//
// Each principal keeps at most capacity mappings. When a new one does not
// fit, the unpinned mapping with the smallest command id gives way and raises the
// principal's floor to that id: a command id at or below the floor that is
// not mapped may have been accepted once, so it is refused as expired rather
// than applied again. Callers that number their commands upwards lose
// nothing but the oldest retries. Accepted revokes remain pinned until durable
// handoff, and an all-pinned row temporarily refuses new publications.

#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H

#include "kernel/command/pending_revoke.h"
#include "kernel/command/work_authority_result.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ayni::kernel {

// Volatile eviction rule; accepted control pins its exact result until handoff.
enum class CommandRetention : uint32_t { normal, accepted_revoke };

// Immutable first result of one accepted command; table owns its canonical bytes.
struct CommandMapping {
    uint64_t principal_id;       // Nonzero authenticated owner of command_id.
    uint64_t command_id;         // Nonzero ID in that principal's retained window
    uint64_t digest;             // ordinary digest, or exact canonical bytes' fast index
    uint64_t commit_seq;         // of the transition that applied it
    ayni_intent_kind_t kind;     // Accepted operation kind; selects canonical/result rules.
    ayni_object_ref_v1 object;   // the result's
    uint32_t canonical_size = 0; // 0 only for legacy ordinary commands
    ayni_intent_effect_t effect =
        AYNI_INTENT_EFFECT_CHANGED; // First result, even after target changes.
    CommandRetention retention = CommandRetention::normal; // volatile; never persisted
    uint32_t result_size = 0; // Historical work authorization bytes; 0 for other command kinds.
};

// Exact mapping syntax, effect/result, accepted flags and digest are checked at each trust
// boundary by this one owner; no catalog or subject lookup is involved.
bool valid_authority_mapping(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                             ayni_bytes_v1 result = {});

// Owns bounded principal windows, canonical storage, pending controls and one
// ephemeral exact-command candidate. Kernel supplies transitions and durable handoff.
class CommandTable {
  public:
    // What find() made of a command.
    enum class Lookup {
        unmapped, // not accepted before, as far as the table knows
        retried,  // accepted before with the same digest: out_mapping
        conflict, // accepted before with another digest
        expired,  // not mapped, at or below the principal's floor
    };
    // Corrupt record versus insufficient configured capacity; neither partially publishes.
    enum class Restore { restored, corrupt, no_room };
    // Borrowed lookup result; pointers live until the next table mutation.
    struct ExactLookup {
        ayni_status_t status; // Request shape/capacity result; lookup meaningful only when OK.
        Lookup lookup;        // Identity outcome; unmapped does not itself accept the command.
        const CommandMapping *mapping;          // Retained first result, or null without one.
        const PendingRevoke *pending = nullptr; // Accepted volatile revoke, or none.
    };

    // Required byte alignment for all table-owned arena regions.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    // rows principals with capacity mappings each.
    static std::size_t storage_size(uint32_t rows, uint32_t capacity,
                                    uint32_t canonical_max_bytes = 0,
                                    uint32_t pending_capacity = 0);

    // storage holds storage_size(rows, capacity) bytes aligned to
    // storage_alignment and outlives the table; capacity is at least 1.
    CommandTable(void *storage, uint32_t rows, uint32_t capacity, uint32_t canonical_max_bytes = 0,
                 uint32_t pending_capacity = 0);

    // Mutable arena ownership cannot be copied.
    CommandTable(const CommandTable &) = delete;
    CommandTable &operator=(const CommandTable &) = delete;

    // Ordinary digest identity; only retried publishes out_mapping.
    Lookup find(uint64_t principal_id, uint64_t command_id, uint64_t digest,
                const CommandMapping *&out_mapping) const;
    // Measures the independent raw request before looking up identity. An
    // unmapped AYNI_OK result stages its canonical bytes in owned scratch;
    // insert_exact consumes them without an intervening table mutation.
    ExactLookup find_exact(uint64_t principal_id, uint64_t command_id,
                           const CanonicalCommandView &input);
    // Approval uses the same exact identity window and staging invariant.
    ExactLookup find_exact(uint64_t principal_id, uint64_t command_id,
                           const ApprovalCommand &input);
    // Revoke lookup clears any create candidate but does not stage one;
    // acceptance copies the request into a separate pending control (ADR-0031).
    ExactLookup find_exact(uint64_t principal_id, uint64_t command_id,
                           const CanonicalRevokeView &input);
    // Consume the exact candidate and copy optional historical result bytes in
    // the same configured byte stride. Refusal preserves the window/floor.
    bool insert_exact(CommandMapping mapping, ayni_bytes_v1 result = {});
    // Checks capacity including pinned mappings, without changing any floor.
    bool can_insert(uint64_t principal_id) const;
    // Identity is always pending-first. A non-revoke kind conflicts with a
    // pending revoke, independently of any ordinary hash/floor.
    Lookup find_pending(uint64_t principal_id, uint64_t command_id, ayni_intent_kind_t kind,
                        ayni_bytes_v1 canonical, const PendingRevoke *&out_pending) const;
    // Borrowed control by exact identity; null when absent, unaffected by the window floor.
    const PendingRevoke *pending(uint64_t principal_id, uint64_t command_id) const;
    // Borrowed mapped result by identity; null when absent.
    const CommandMapping *mapped(uint64_t principal_id, uint64_t command_id) const;
    // Checks identity/target/capacity without mutation. No Session, Grant
    // lifecycle or storage admission belongs to the command owner.
    ayni_status_t can_accept_revoke(const AcceptedRevoke &accepted) const;
    // Copy a fully admitted identity into one dedicated control; failure preserves out_pending.
    ayni_status_t accept_revoke(const AcceptedRevoke &accepted, const PendingRevoke *&out_pending);
    // Smallest target ID among unapplied controls, or null when none remain.
    const PendingRevoke *next_pending_revoke() const;
    // This owned pending control has room in the one result window.
    bool can_publish_accepted(const PendingRevoke &pending) const;
    // Bypasses floor only for an already accepted control. Copies to the
    // one result window, pins it, and changes the control to APPLIED.
    bool publish_accepted_revoke(const PendingRevoke &pending, CommandMapping mapping,
                                 uint64_t boot_epoch);
    // Earliest queued sequence confirmed by this exact boot, then target ID; null if none.
    const PendingRevoke *next_durable_pending(uint64_t boot_epoch, uint64_t durable_seq) const;
    // During completed recovery compare restored exact bytes, never an old
    // queued seq against the new durable floor. Does not consult pending/floor.
    Lookup recovered_revoke(const PendingRevoke &pending, const CommandMapping *&out_mapping) const;
    // Kernel first hands the deny gate to durable authority, then retires.
    bool retire_pending(uint64_t principal_id, uint64_t command_id);
    uint32_t pending_size() const; // Number of accepted controls, including APPLIED pins.
    // All-kind readonly receipt lookup. For incomplete recovery the Kernel
    // may read pending() alone and withhold the partial restored window.
    Lookup lookup_receipt(uint64_t principal_id, uint64_t command_id,
                          const PendingRevoke *&out_pending,
                          const CommandMapping *&out_mapping) const;
    // Keep accepted identities but invalidate every old boot/sequence confirmation.
    void abandon_queued_for_reload();
    void clear_restored(); // resets window/scratch and queued stamps, retains accepted controls
    // mapping must be a reference obtained from this table, not a copy.
    ayni_bytes_v1 canonical(const CommandMapping &mapping) const;
    // Same borrowed lifetime as canonical(); empty only when result_size is 0.
    ayni_bytes_v1 authorization_result(const CommandMapping &mapping) const;
    // Whether insert() has a row for the principal.
    bool has_row(uint64_t principal_id) const;
    // Maps a command that find() returned unmapped for, giving way as above.
    // Only after can_insert(mapping.principal_id); false changes no window/floor.
    bool insert(const CommandMapping &mapping);
    // The mapping insert() or restore() added with commit_seq, if any.
    const CommandMapping *mapped_in(uint64_t commit_seq) const;
    // The principal's floor; 0 before anything gave way.
    uint64_t floor(uint64_t principal_id) const;

    // Replays a COMMAND_MAPPING record: the mapping, which may give way as
    // insert() does, and the floor it left. Returns false, changing
    // nothing, for a mapping the table already has or with no row left.
    bool restore(const CommandMapping &mapping, uint64_t floor);
    // Classified restore, with exact authority bytes when the record requires them.
    Restore restore_checked(const CommandMapping &mapping, uint64_t floor,
                            ayni_bytes_v1 canonical = {}, ayni_bytes_v1 result = {});
    uint32_t size() const; // Total retained mappings across principals, not pending controls.
    const CommandMapping &at(uint32_t index) const; // Flattened retained index, below size().
    // Full reset including accepted controls. Recovery discard uses clear_restored().
    void clear();

  private:
    // Owns one principal window and its monotone evicted-command floor.
    struct Row {
        uint64_t principal_id; // 0: unused; otherwise the mappings' shared owner.
        uint64_t floor;        // Inclusive expired command ID; 0 before any eviction.
        uint32_t count;        // Retained mappings in this row, at most capacity_.
    };

    // One lookup/stage algorithm for two real exact request codecs.
    template <typename Input>
    ExactLookup
    find_candidate(uint64_t principal_id, uint64_t command_id, ayni_intent_kind_t kind,
                   const Input &input, ayni_status_t (*measure)(const Input &, uint32_t &),
                   ayni_status_t (*encode)(const Input &, uint8_t *, uint32_t, uint32_t &));
    Row *row_of(uint64_t principal_id) const;
    Row *new_row(uint64_t principal_id);
    CommandMapping *mappings_of(const Row &row) const;
    bool add(Row &row, const CommandMapping &mapping, ayni_bytes_v1 canonical,
             ayni_bytes_v1 result = {});
    PendingRevoke *pending_slot(uint64_t principal_id, uint64_t command_id) const;
    ExactLookup find_revoke_bytes(uint64_t principal_id, uint64_t command_id, ayni_bytes_v1 bytes);
    bool owns_pending(const PendingRevoke &pending) const;
    uint8_t *canonical_of(const CommandMapping &mapping) const;

    Row *rows_;
    CommandMapping *mappings_;
    PendingRevoke *pending_;
    uint8_t *canonical_;
    uint8_t *scratch_;
    uint32_t row_count_;
    uint32_t capacity_;
    uint32_t canonical_max_bytes_; // Per-slot input plus result byte bound; also scratch capacity.
    uint32_t pending_capacity_;
    // One admitted exact request for the current scratch bytes. Revocation
    // owns its canonical bytes in PendingRevoke and never stages a create.
    struct StagedCommand {
        uint64_t principal_id;   // Nonzero authenticated command owner.
        uint64_t command_id;     // Nonzero identity, paired with principal_id.
        uint64_t digest;         // Fast index of scratch; equality still compares bytes.
        uint32_t size;           // Positive canonical byte count, <= canonical_max_bytes_.
        ayni_intent_kind_t kind; // Grant creation or Goal approval; selects result proof.
    };
    // Disengaged means no candidate. Ordinary insert, restore, clear and every
    // exact lookup invalidate the entire value, never a length-only flag.
    std::optional<StagedCommand> staged_;
    // The latest mapping added and the commit_seq it was added with.
    uint64_t last_added_seq_ = 0;
    uint64_t last_added_principal_ = 0;
    uint64_t last_added_command_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_TABLE_H
