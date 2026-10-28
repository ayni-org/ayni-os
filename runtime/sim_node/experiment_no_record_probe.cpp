// Production builds have no Core-internal injection probe. Unsupported paths
// fail explicitly rather than substituting a different cut or reporting success.
#include "experiment_composite.h"
#include "experiment_support.h"
namespace ayni::runtime::experiments {
bool replace_storage_version(World &world, VersionFault, uint16_t) {
    return world.evidence.status("version probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
ayni_status_t inspect_task_progress(const ayni_kernel_t *, uint64_t, TaskProgressEvidence &) {
    return AYNI_UNSUPPORTED;
}
ayni_status_t inspect_call_route(const ayni_kernel_t *, uint64_t, CallRouteEvidence &) {
    return AYNI_UNSUPPORTED;
}
bool verify_completion_slots(Evidence &e) {
    return e.status("completion probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
bool World::hold_record_append(RecordPoint, const char *) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
bool World::crash_after_record(RecordPoint, const char *) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
bool World::damage_log(Damage) {
    return evidence.status("record probe requires AYNI_BUILD_TESTS", AYNI_UNSUPPORTED);
}
} // namespace ayni::runtime::experiments
