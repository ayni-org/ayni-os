#include "catalog/goal_type/work_profile.h"

namespace ayni::catalog {
bool WorkProfile::decode(const ayni_work_profile_v1 &input, WorkProfile &out) {
    if (input.reserved != 0U) {
        return false;
    }
    WorkProfile profile{input.work_class, input.background_period_min_us};
    if (profile.kind == AYNI_WORK_BACKGROUND && profile.background_period_us == 0U) {
        profile.background_period_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US;
    }
    if (!profile.valid()) {
        return false;
    }
    out = profile;
    return true;
}
bool WorkProfile::valid() const {
    if (kind > AYNI_WORK_BACKGROUND) {
        return false;
    }
    return kind == AYNI_WORK_BACKGROUND ? background_period_us >= AYNI_TRIGGER_PERIOD_MIN_US &&
                                              background_period_us != UINT64_MAX
                                        : background_period_us == 0U;
}
bool WorkProfile::accepts(ayni_trigger_kind_t trigger, uint64_t period_us) const {
    return kind != AYNI_WORK_BACKGROUND ||
           (trigger == AYNI_TRIGGER_INTERVAL && period_us >= background_period_us);
}
} // namespace ayni::catalog
