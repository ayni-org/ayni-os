// Trusted scheduling semantics, retained by each Goal independently of policy output.
#ifndef AYNI_CORE_CATALOG_GOAL_TYPE_WORK_PROFILE_H
#define AYNI_CORE_CATALOG_GOAL_TYPE_WORK_PROFILE_H
#include <ayni/ayni.h>

namespace ayni::catalog {
// Owns a normalized class and its background period floor; no runtime queue state.
struct WorkProfile {
    ayni_work_class_t kind = AYNI_WORK_ORDINARY; // Trusted class, never proposed by a model.
    // Reboot recovery policy: RESUME (0) or CANCEL_ON_REBOOT (1, ADR-0110).
    ayni_goal_recovery_t recovery = AYNI_GOAL_RECOVERY_RESUME;
    uint64_t background_period_us =
        0; // Zero outside BACKGROUND; positive finite microseconds there.
    // Decode once at the ABI boundary; zero selects the documented background default.
    static bool decode(const ayni_work_profile_v1 &input, WorkProfile &out);
    // Intrinsic validation shared by live and persisted representations.
    bool valid() const;
    // Background work must use a periodic trigger no faster than the trusted floor.
    bool accepts(ayni_trigger_kind_t trigger, uint64_t period_us) const;
    // Reminders retain pending occurrences; background derivation also has a finite wait.
    bool accepts_occurrence_wait(uint64_t wait_us) const {
        return kind == AYNI_WORK_REMINDER     ? wait_us == AYNI_OCCURRENCE_WAIT_UNBOUNDED
               : kind == AYNI_WORK_BACKGROUND ? wait_us <= AYNI_RESOURCE_WAIT_DEFAULT_US
                                              : true;
    }
    // Default retains ordinary durable recovery and call behavior.
    bool is_default() const {
        return kind == AYNI_WORK_ORDINARY && recovery == AYNI_GOAL_RECOVERY_RESUME &&
               background_period_us == 0;
    }
    // True when unfinished work is cancelled on a new boot and may run EPHEMERAL calls.
    bool cancel_on_reboot() const { return recovery == AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT; }
    // Complete immutable semantic identity, used when resolving a Goal after restart.
    bool operator==(const WorkProfile &other) const {
        return kind == other.kind && recovery == other.recovery &&
               background_period_us == other.background_period_us;
    }
    // Copied diagnostics; no catalog pointer escapes.
    ayni_work_profile_v1 snapshot() const { return {kind, recovery, background_period_us}; }
};
} // namespace ayni::catalog
#endif
