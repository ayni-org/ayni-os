// Persistent bounded privacy sweep. Work/knowledge owners erase their own representation;
// this state owns only restartable progress and the two-slot storage completion barrier.
#ifndef AYNI_CORE_WORK_PRIVACY_CLEANUP_H
#define AYNI_CORE_WORK_PRIVACY_CLEANUP_H
#include "foundation/transition_stamp.h"
#include <ayni/ayni.h>
namespace ayni::work {
// A request restarts at memories; each subsequent phase uses an exclusive stable-ID cursor.
enum class PrivacyPhase : uint32_t {
    idle = 0,
    memories = 1,
    contexts = 2,
    goals = 3,
    tasks = 4,
    calls = 5,
    commands = 6,
    files = 7,
    compact = 8,
    purge = 9,
    complete = 10
};
// The last progress transition's one owner mutation, or none for a scan/phase advance.
enum class PrivacyEffect : uint32_t {
    none = 0,
    memory = 1,
    context = 2,
    goal = 3,
    task = 4,
    call = 5,
    command = 6,
    remove_memory = 7
};
// Persisted sweep fact; all-zero is the never-requested state and is never serialized.
struct PrivacyRecord {
    uint64_t request_seq = 0; // Latest accepted invalidation's positive commit sequence.
    PrivacyPhase phase = PrivacyPhase::idle;    // Completion means all owners/files/slots cleared.
    PrivacyEffect effect = PrivacyEffect::none; // Applies to cursor in this exact transition.
    uint64_t cursor = 0;              // Last scanned stable ID in this phase; zero begins a phase.
    uint64_t snapshot_generation = 0; // Target generation from compact through complete, else zero.
    foundation::TransitionStamp stamp{}; // Latest request/progress transition, zero iff idle.
};
// One sweep covers all invalid retained content; a new invalidation resets its progress.
class PrivacyCleanup {
  public:
    static constexpr uint64_t snapshot_passes =
        2; // Both managed snapshot slots must be overwritten.
    // Immutable persisted progress; never a caller-owned mutable cursor.
    const PrivacyRecord &record() const { return record_; }
    bool pending() const {
        return record_.phase != PrivacyPhase::idle && record_.phase != PrivacyPhase::complete;
    }
    // Clearing a Context can invalidate previously scanned sources; one bounded rescan follows.
    bool source_rescan_required() const {
        return record_.phase == PrivacyPhase::contexts && record_.effect == PrivacyEffect::context;
    }
    // Same transition as the accepted source revision/deletion, before any later cleanup.
    void request(foundation::TransitionStamp stamp);
    // Publish one preflighted scan item or phase advance; no implicit allocation or I/O.
    void advance(PrivacyPhase phase, uint64_t cursor, PrivacyEffect effect,
                 uint64_t snapshot_generation, foundation::TransitionStamp stamp);
    // Shared intrinsic record checks. History validity is independent of current catalog/clock.
    static bool valid(const PrivacyRecord &record);
    // Prove monotone request/progress before mutation; replay separately proves its one effect.
    bool can_restore(const PrivacyRecord &record) const;
    void restore_validated(const PrivacyRecord &record) { record_ = record; }
    void clear() { record_ = {}; }

  private:
    PrivacyRecord record_{}; // Sole progress owner; no retained plaintext or model pointers.
};
} // namespace ayni::work
#endif
