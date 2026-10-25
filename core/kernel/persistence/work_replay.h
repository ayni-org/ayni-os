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
// that registers them again. Schema 2 requires complete Goal/Task admission
// facts. Compatible work resumes through the ordinary authority checks; expiry,
// revocation and ancestor binding changes still deny consumption. Times are on
// one boot's clock, so each wait starts again in full when recovery ends.

#ifndef AYNI_CORE_KERNEL_PERSISTENCE_WORK_REPLAY_H
#define AYNI_CORE_KERNEL_PERSISTENCE_WORK_REPLAY_H

#include "durability/batch/prefix_scan.h"
#include "kernel/intent/work_model.h"
#include "kernel/persistence/persistent_store.h"

#include <cstdint>
#include <optional>

namespace ayni::kernel {

class WorkReplay {
  public:
    // A durable binding is authoritative. Without one, only an empty or
    // agent-only store may acquire its first owner; existing owner-controlled
    // work or commands cannot be adopted from the Host configuration.
    bool binding_matches() const;
    // Retained Scene command/state relation, shared with snapshots; no startup catalog lookup.
    bool matches_scene_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) const;
    // model outlives the replay.
    explicit WorkReplay(WorkModel &model) : model_(model) {}

    WorkReplay(const WorkReplay &) = delete;
    WorkReplay &operator=(const WorkReplay &) = delete;

    // context is the replay. Boot supplies the epoch preceding this record;
    // a later epoch or binding replacement must not legitimize a future ID or
    // ingress incarnation in history.
    static durability::RecordUse on_record(void *context, const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body, uint64_t boot_epoch_limit);
    // applied: a known type/version whose header is accepted; corrupt:
    // an authority or store record lacks REQUIRED (owner binding, Grant
    // create/state, revocation/progress, command v2+ or any type of
    // persistent_store.h); unknown: unsupported type/version.
    // Discovery uses the same classification before skipping covered data;
    // applied does not validate the body.
    static durability::RecordUse
    classify_header(const durability::RecordHeader &header,
                    WorkRecordContext context = WorkRecordContext::journal);
    // Immutable evidence shared by journal pairing and retained snapshot facts.
    static bool matches_grant_creation(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                       const authority::Grant &grant, ayni_bytes_v1 domain,
                                       ayni_bytes_v1 scope);

    // Snapshot proof allows legitimately reclaimed history; journal pairing
    // separately requires every new Grant, dependency and activation record.
    bool matches_goal_approval(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                               ayni_bytes_v1 result_bytes, uint64_t transition_limit = UINT64_MAX,
                               uint64_t boot_epoch_limit = UINT64_MAX) const;

    // Historical reauthorization result uses its saved choices, never current references.
    bool matches_goal_reauthorization(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                      ayni_bytes_v1 result_bytes,
                                      uint64_t transition_limit = UINT64_MAX,
                                      uint64_t boot_epoch_limit = UINT64_MAX) const;

    // Every created Goal/Task needs its state; Grant creation/root revocation
    // needs its exact command, and revoked children need same-batch progress.
    bool complete() const { return pairing_.complete() && calls_consistent(UINT64_MAX); }
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
                                const durability::RecordHeader &header, const uint8_t *body,
                                uint64_t boot_epoch_limit);
    // Dispatch while batch_seq_ still names the previously consumed record.
    durability::RecordUse apply_record(const durability::BatchIdentity &batch,
                                       const durability::RecordHeader &header, const uint8_t *body,
                                       uint64_t boot_epoch_limit);
    durability::RecordUse restore_scene(const uint8_t *body, uint32_t size,
                                        foundation::TransitionStamp stamp,
                                        uint64_t boot_epoch_limit);
    durability::RecordUse restore_owner_binding(const uint8_t *body, uint32_t size,
                                                foundation::TransitionStamp stamp);
    durability::RecordUse restore_instance(const uint8_t *body, uint32_t size,
                                           foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal(uint16_t version, const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal_state(uint16_t version, const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    // Goal v2 must receive complete metadata in the same journal batch.
    durability::RecordUse restore_goal_authority(uint16_t version, const uint8_t *body,
                                                 uint32_t size, foundation::TransitionStamp stamp,
                                                 uint64_t boot_epoch_limit);
    // Complete work scan footer: exact progress/request plus any pause cohort.
    durability::RecordUse restore_authority_cleanup(const uint8_t *body, uint32_t size,
                                                    foundation::TransitionStamp stamp);
    durability::RecordUse restore_task(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_authority(const uint8_t *body, uint32_t size,
                                                 foundation::TransitionStamp stamp);
    // Full row must match this batch and its owning Task; never reissues I/O.
    durability::RecordUse restore_call(uint16_t version, const uint8_t *body, uint32_t size,
                                       work::ActionStamp stamp);
    // Every retained selected call and terminal outcome agrees with its Task.
    bool calls_consistent(uint64_t boot_epoch_limit) const;
    // Closes the exact query acceptance with its durable owner's canonical command.
    bool pair_target_command(const CommandMapping &mapping, ayni_bytes_v1 canonical);
    bool pair_task_limit_command(const CommandMapping &mapping,
                                 ayni_bytes_v1 canonical); // Closes a ceiling raise.
    bool pair_scene_command(const CommandMapping &mapping,
                            ayni_bytes_v1 canonical); // Closes one preference change.
    durability::RecordUse restore_task_state(uint16_t version, const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_steps(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_checkpoint(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp);
    // A create uses an ID of the epoch preceding it, a revocation one of an
    // epoch no later: a later BOOT_EPOCH must not legitimize a future ID.
    durability::RecordUse restore_grant(uint16_t version, const uint8_t *body, uint32_t size,
                                        foundation::TransitionStamp stamp,
                                        uint64_t boot_epoch_limit);
    durability::RecordUse restore_grant_state(uint16_t version, const uint8_t *body, uint32_t size,
                                              foundation::TransitionStamp stamp);
    durability::RecordUse restore_revocation(uint16_t version, const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp,
                                             uint64_t boot_epoch_limit);
    durability::RecordUse restore_revocation_progress(const uint8_t *body, uint32_t size,
                                                      foundation::TransitionStamp stamp);
    durability::RecordUse restore_command(uint16_t version, const uint8_t *body, uint32_t size,
                                          uint64_t commit_seq);
    // One approval's ordered relation, independent of record body decoding.
    // Prove the admitted generation and original parent chain at a known sequence.
    enum class BindingEvidence {
        current,
        historical
    }; // Historical ancestors may be replaced later.
    bool matches_goal_binding(uint32_t index, const work::GoalAuthorityRecord &record,
                              uint64_t admission_seq,
                              BindingEvidence evidence = BindingEvidence::current) const;
    bool pair_approval_grant(const authority::GrantRecord &record,
                             foundation::TransitionStamp stamp);
    bool pair_approval_state(uint32_t index, const work::GoalState &state);
    bool pair_approval_metadata(uint32_t index, const work::GoalAuthorityRecord &record);
    bool pair_approval_command(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                               ayni_bytes_v1 result_bytes);
    bool allows_reauthorization_record(const durability::RecordHeader &header) const;
    bool begin_reauthorization(uint32_t index, foundation::TransitionStamp stamp);
    bool pair_reauthorization_metadata(uint32_t index, const work::GoalAuthorityRecord &record,
                                       foundation::TransitionStamp stamp);
    bool pair_reauthorization_command(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                      ayni_bytes_v1 result_bytes);
    bool reauthorized_cohort_complete(uint32_t index, foundation::TransitionStamp stamp) const;
    // Cross-table facts are checked without consulting the new boot catalog.
    bool goal_authority_consistent(uint32_t goal_index, uint64_t boot_epoch_limit) const;
    bool task_authority_consistent(uint32_t task_index, uint64_t boot_epoch_limit) const;
    // Retained selections share subject/Scope/history checks; reclaimed rows
    // are allowed only by callers validating terminal or historical evidence.
    enum class SelectionOrigin {
        existing_grant,
        approval
    }; // Only approval may issue same-batch roots.
    bool matches_goal_selection(uint32_t goal_index, const work::GoalRequirement &need,
                                uint32_t grant_index, uint64_t admission_seq,
                                SelectionOrigin origin) const;
    bool saved_references_consistent(
        uint32_t goal_index, const work::GoalAuthorityRecord &ceiling,
        const std::array<work::AuthorityReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> &references,
        uint64_t admission_seq) const;
    void hold_tasks();

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
    // Transient batch proof. Grant IDs are unordered because journal slot order
    // can differ from requirement order after safe historical-slot reuse.
    struct ApprovalPair {
        enum class Stage { grants, state, metadata, complete };
        Stage stage = Stage::grants; // metadata awaits command; complete seals the batch.
        uint64_t goal_id = 0;    // Exact proposal identity, nonzero while the optional is engaged.
        uint32_t goal_index = 0; // Original occupied slot, never replaced inside this batch.
        uint32_t revision = 0;   // Original proposal revision before activation.
        uint32_t missing_mask = 0; // Original absent dependencies; never expanded on replay.
        uint32_t count = 0;        // Prefix of new grant_ids, at most the original missing count.
        std::array<uint64_t, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> grant_ids{};
    };
    // Batch pairing is replay-owned transient state, never serialized. Goal
    // and Task pairs may coexist with authority; disengaged means no missing
    // state. progress_seq remembers the last progress batch to reject duplicates.
    struct BatchPairing {
        // One replacement's before-image identifiers survive metadata publication.
        // Task state protects its own ordinary fields; footer closes the whole cohort.
        struct ReauthorizationPair {
            enum class Stage { state, metadata, command };
            Stage stage = Stage::state; // Exact Goal state -> metadata/tasks -> command/footer.
            uint64_t goal_id = 0;       // Exact retained target, positive when optional is engaged.
            uint32_t goal_index = 0; // Stable during this batch; identifies the one mutable Goal.
            uint32_t revision = 0;   // Positive user revision before the replacement.
            uint64_t authority_seq = 0; // Last authority fact's original positive CAS.
            uint64_t binding_seq = 0;   // Original generation expected by every direct live Task.
        };
        std::optional<ReauthorizationPair>
            reauthorization; // Disengaged outside a replacement batch.
        // One optional work cleanup proof. A paused Goal additionally requires
        // every direct live Task's independent deny fact before the footer.
        struct WorkCleanupPair {
            enum class Stage { none, required, complete };
            Stage stage = Stage::none;   // Required awaits type19; complete seals this batch.
            uint64_t paused_goal_id = 0; // Exact cohort Goal, or 0 when no cohort is required.
            uint64_t ended_goal_id = 0; // Exact scanned dedicated-Grant owner, or 0 before any end.
            uint64_t state_goal_id = 0; // Goal with immutable user state awaiting pause metadata.
            void require() { stage = Stage::required; }
            bool pending() const { return stage == Stage::required; }
        } work_cleanup;
        // A journal transition derives at most one Task, even after that
        // Task's complete state/admission pair has closed. 0 before any.
        uint64_t task_created_seq = 0;
        std::optional<uint64_t> goal; // Nonzero created Goal awaiting its state; empty otherwise.
        std::optional<uint64_t> task_authority; // Exact new Task awaiting its admission record.
        std::optional<uint64_t> task; // Nonzero created Task awaiting its state; empty otherwise.
        std::optional<uint64_t> goal_authority; // Goal v2 awaiting its explicit ceiling.
        uint64_t goal_authority_seq = 0; // Last type 17 batch; one Goal authority change per batch.
        std::optional<ApprovalPair> approval; // Retains a closed proof until the next batch.
        AuthorityPair authority;              // Exactly one authority alternative or none.
        uint64_t progress_seq = 0; // Last progress commit sequence; 0 before any progress.
        // A standalone Task ceiling change closes only with its exact owner command.
        struct TaskLimitPair {
            enum class Stage { state, command };
            TaskLimitCommand request;   // Before-CAS and new ceiling from the paired state.
            Stage stage = Stage::state; // command seals this batch until the next identity.
        };
        std::optional<TaskLimitPair> limit_raise; // Empty outside runtime-limit management.
        struct TargetCommandPair {
            TargetCommand request;     // Exact before-CAS and Call/Task target identity.
            bool command_seen = false; // Acceptance cannot finish without its paired owner command.
        };
        struct ScenePair {
            uint64_t object_id; // Exact changed state awaiting its canonical owner command.
            bool command_seen;  // True seals the standalone transition until the next batch.
        };
        std::optional<ScenePair>
            scene; // Empty for non-Scene or no-change command-only transitions.
        std::optional<TargetCommandPair> target_command; // Only this batch's query or wait resume.
        // Existing work/command partners, independently of the new scan footer.
        bool partners_complete() const {
            return (!scene || scene->command_seen) &&
                   (!target_command || target_command->command_seen) &&
                   (!limit_raise || limit_raise->stage == TaskLimitPair::Stage::command) && !goal &&
                   !task && !task_authority && !goal_authority && !authority.pending() &&
                   (!approval || approval->stage == ApprovalPair::Stage::complete) &&
                   (!reauthorization ||
                    reauthorization->stage == ReauthorizationPair::Stage::command);
        }
        // Every accepted pair in the batch has received its required partner.
        bool complete() const { return partners_complete() && !work_cleanup.pending(); }
    };
    BatchPairing pairing_; // Clear the entire state on discard; never reuse across a reload.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_PERSISTENCE_WORK_REPLAY_H
