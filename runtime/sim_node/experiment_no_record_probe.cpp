// Production builds have no Core-internal injection probe. Unsupported paths
// fail explicitly rather than substituting a different cut or reporting success.
#include "experiment_support.h"
namespace ayni::runtime::experiments {
bool World::hold_record_append(RecordPoint) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
bool World::crash_after_record(RecordPoint) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
bool World::damage_log(Damage) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
} // namespace ayni::runtime::experiments
