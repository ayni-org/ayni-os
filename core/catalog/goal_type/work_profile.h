// Trusted scheduling semantics, retained by each Goal independently of policy output.
#ifndef AYNI_CORE_CATALOG_GOAL_TYPE_WORK_PROFILE_H
#define AYNI_CORE_CATALOG_GOAL_TYPE_WORK_PROFILE_H
#include <ayni/ayni.h>

namespace ayni::catalog {
// Owns a normalized class and its background period floor; no runtime queue state.
struct WorkProfile {
    ayni_work_class_t kind = AYNI_WORK_ORDINARY; // Trusted class, never proposed by a model.
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
    // Default retains the old Goal record layout and ordinary call behavior.
    bool is_default() const { return kind == AYNI_WORK_ORDINARY && background_period_us == 0; }
    // Complete immutable semantic identity, used when resolving a Goal after restart.
    bool operator==(const WorkProfile &other) const {
        return kind == other.kind && background_period_us == other.background_period_us;
    }
    // Copied diagnostics; no catalog pointer escapes.
    ayni_work_profile_v1 snapshot() const { return {kind, 0, background_period_us}; }
};
} // namespace ayni::catalog
#endif
