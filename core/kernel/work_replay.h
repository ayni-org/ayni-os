// Recovery of the work model from the log (ADR-0026). The boot sequence
// hands every record it does not own to on_record() as it reads the log, in
// log order, so the tables pass through the state each stored transition
// left. A record that breaks the rules of its table is corruption, and one
// the capacities of this boot cannot hold is no_room; either ends recovery
// in RESTRICTED, and the kernel then discards everything replayed.
//
// Once the log is read and this boot's epoch is durable, finish() fits what
// came back to this boot: goal types, strategies and agents are registered
// anew each boot, so a goal whose type is gone, a task whose checkpoint its
// strategy cannot read and an instance whose agent is gone are held or
// FAULTED in memory only, and the log keeps them as they were for a boot
// that registers them again. Work without the authorization records that
// MA-006 will add is held for explicit confirmation. Times are on one boot's
// clock, so each wait starts again in full when recovery ends.

#ifndef AYNI_CORE_KERNEL_WORK_REPLAY_H
#define AYNI_CORE_KERNEL_WORK_REPLAY_H

#include "durability/batch/prefix_scan.h"
#include "kernel/intent/work_model.h"

#include <cstdint>
#include <optional>

namespace ayni::kernel {

enum class WorkRecordContext { journal, snapshot };

class WorkReplay {
  public:
    // A durable binding is authoritative. Before the first binding, every
    // retained authenticated owner fact must agree with the Host; recovery
    // must not turn a legacy store into an implicit ownership transfer.
    bool binding_matches() const;
    // model outlives the replay.
    explicit WorkReplay(WorkModel &model) : model_(model) {}

    WorkReplay(const WorkReplay &) = delete;
    WorkReplay &operator=(const WorkReplay &) = delete;

    // A durability::RecordHandler; context is the replay.
    static durability::RecordUse on_record(void *context, const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body);
    // applied: a known type/version whose header is accepted; corrupt:
    // an authority record lacks REQUIRED (owner binding, Grant create/state,
    // revocation/progress, or command v2+); unknown: unsupported type/version.
    // Discovery uses the same classification before skipping covered data;
    // applied does not validate the body.
    static durability::RecordUse
    classify_header(const durability::RecordHeader &header,
                    WorkRecordContext context = WorkRecordContext::journal);
    // Immutable evidence shared by journal pairing and retained snapshot facts.
    static bool matches_grant_creation(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                       const authority::Grant &grant, ayni_bytes_v1 domain,
                                       ayni_bytes_v1 scope);

    // Every created Goal/Task needs its state; Grant creation/root revocation
    // needs its exact command, and revoked children need same-batch progress.
    bool complete() const { return pairing_.complete(); }
    // After the last batch: whether the tables hold a state a transition
    // leaves, beyond what each record's own checks cover.
    bool consistent(uint64_t boot_epoch_limit = UINT64_MAX) const;
    // Only once complete() and consistent(): holds and faults what this boot
    // lacks, then puts every wait on the clock at now_us.
    void finish(uint64_t now_us);
    // Drops everything replayed, as when recovery fails.
    void discard();
    // Reload retains registration, but no session or cached due-work from
    // the discarded boot may reach the recovered model.
    void discard_for_reload();

  private:
    durability::RecordUse apply(const durability::BatchIdentity &batch,
                                const durability::RecordHeader &header, const uint8_t *body);
    durability::RecordUse restore_owner_binding(const uint8_t *body, uint32_t size,
                                                foundation::TransitionStamp stamp);
    durability::RecordUse restore_instance(const uint8_t *body, uint32_t size,
                                           foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal(uint16_t version, const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal_state(uint16_t version, const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    // Goal v2 must receive complete metadata in the same journal batch.
    durability::RecordUse restore_goal_authority(const uint8_t *body, uint32_t size,
                                                 foundation::TransitionStamp stamp);
    durability::RecordUse restore_task(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_state(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_steps(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_checkpoint(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp);
    durability::RecordUse restore_grant(uint16_t version, const uint8_t *body, uint32_t size,
                                        foundation::TransitionStamp stamp);
    durability::RecordUse restore_grant_state(uint16_t version, const uint8_t *body, uint32_t size,
                                              foundation::TransitionStamp stamp);
    durability::RecordUse restore_revocation(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_revocation_progress(const uint8_t *body, uint32_t size,
                                                      foundation::TransitionStamp stamp);
    durability::RecordUse restore_command(uint16_t version, const uint8_t *body, uint32_t size,
                                          uint64_t commit_seq);
    // Cross-table facts are checked without consulting the new boot catalog.
    bool goal_authority_consistent(uint32_t goal_index, uint64_t boot_epoch_limit) const;
    void hold_tasks();
    void hold_unconfirmed_work();

    WorkModel &model_;
    uint64_t batch_seq_ = 0; // of the batch the last record came in
    // One unmatched authority record. These alternatives cannot coexist in a
    // batch: a create/root revoke awaits its command; a child awaits progress.
    enum class AuthorityPairKind { none, create, revocation, cleanup_child };
    // Payload fields are valid only for the selected alternative; all other fields are zero.
    struct AuthorityPair {
        AuthorityPairKind kind = AuthorityPairKind::none; // Selects the valid fields below.
        uint64_t grant_id = 0; // Nonzero target/child iff kind is not none.
        uint64_t root_id = 0;  // Nonzero only for cleanup_child, the child's original revoker.
        uint32_t revision = 0; // Root result revision; TARGET_GONE legitimately uses 0.
        ayni_intent_effect_t effect = AYNI_INTENT_EFFECT_NONE; // Result only for revocation.
        // Whether this batch still lacks an authority partner record.
        bool pending() const { return kind != AuthorityPairKind::none; }
        // Cleanup pairs with progress, whereas create/root revoke pair with a command.
        bool awaiting_command() const {
            return kind == AuthorityPairKind::create || kind == AuthorityPairKind::revocation;
        }
    };
    // Batch pairing is replay-owned transient state, never serialized. Goal
    // and Task pairs may coexist with authority; disengaged means no missing
    // state. progress_seq remembers the last progress batch to reject duplicates.
    struct BatchPairing {
        std::optional<uint64_t> goal; // Nonzero created Goal awaiting its state; empty otherwise.
        std::optional<uint64_t> task; // Nonzero created Task awaiting its state; empty otherwise.
        std::optional<uint64_t> goal_authority; // Goal v2 awaiting its explicit ceiling.
        uint64_t goal_authority_seq = 0; // Last type 17 batch; one Goal authority change per batch.
        AuthorityPair authority;         // Exactly one authority alternative or none.
        uint64_t progress_seq = 0;       // Last progress commit sequence; 0 before any progress.
        // Every accepted pair in the batch has received its required partner.
        bool complete() const { return !goal && !task && !goal_authority && !authority.pending(); }
    };
    BatchPairing pairing_; // Clear the entire state on discard; never reuse across a reload.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_WORK_REPLAY_H
