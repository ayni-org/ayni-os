#include "work/privacy/privacy_cleanup.h"
namespace ayni::work {
namespace {
// Explicit phase/effect relation; enum ordinals never imply cross-owner mutation rights.
bool effect_allowed(PrivacyPhase phase, PrivacyEffect effect) {
    if (effect == PrivacyEffect::none) {
        return true;
    }
    switch (phase) {
    case PrivacyPhase::memories:
        return effect == PrivacyEffect::memory;
    case PrivacyPhase::contexts:
        return effect == PrivacyEffect::context;
    case PrivacyPhase::goals:
        return effect == PrivacyEffect::goal;
    case PrivacyPhase::tasks:
        return effect == PrivacyEffect::task;
    case PrivacyPhase::calls:
        return effect == PrivacyEffect::call;
    case PrivacyPhase::commands:
        return effect == PrivacyEffect::command;
    case PrivacyPhase::purge:
        return effect == PrivacyEffect::remove_memory;
    default:
        return false;
    }
}
} // namespace
void PrivacyCleanup::request(foundation::TransitionStamp stamp) {
    record_ = {stamp.commit_seq, PrivacyPhase::memories, PrivacyEffect::none, 0, 0, stamp};
}
void PrivacyCleanup::advance(PrivacyPhase phase, uint64_t cursor, PrivacyEffect effect,
                             uint64_t generation, foundation::TransitionStamp stamp) {
    record_ = {record_.request_seq, phase, effect, cursor, generation, stamp};
}
bool PrivacyCleanup::valid(const PrivacyRecord &r) {
    if (r.request_seq == 0 || r.stamp.transition_id == 0 || r.stamp.commit_seq < r.request_seq ||
        r.phase < PrivacyPhase::memories || r.phase > PrivacyPhase::complete ||
        r.effect > PrivacyEffect::remove_memory) {
        return false;
    }
    const bool storage = r.phase >= PrivacyPhase::compact;
    if (storage != (r.snapshot_generation != 0) ||
        ((r.phase == PrivacyPhase::files || r.phase == PrivacyPhase::compact ||
          r.phase == PrivacyPhase::complete) &&
         r.cursor != 0) ||
        (r.effect != PrivacyEffect::none &&
         (r.cursor == 0 || !effect_allowed(r.phase, r.effect)))) {
        return false;
    }
    // New requests have one canonical reset representation; they cannot claim prior cleanup.
    return r.request_seq != r.stamp.commit_seq ||
           (r.phase == PrivacyPhase::memories && r.effect == PrivacyEffect::none && r.cursor == 0);
}
// Stable IDs avoid dependence on moving Goal/Memory snapshot row positions. A new accepted
// invalidation restarts the walk; Context erasure alone permits a fixed-point rewind.
bool PrivacyCleanup::can_restore(const PrivacyRecord &r) const {
    if (!valid(r)) {
        return false;
    }
    if (record_.phase == PrivacyPhase::idle) {
        return true;
    }
    if (r.stamp.commit_seq <= record_.stamp.commit_seq ||
        r.stamp.transition_id <= record_.stamp.transition_id ||
        r.request_seq < record_.request_seq) {
        return false;
    }
    if (r.request_seq > record_.request_seq) {
        return r.request_seq == r.stamp.commit_seq;
    }
    if (source_rescan_required() && r.phase == PrivacyPhase::memories) {
        return r.effect == PrivacyEffect::none && r.cursor == 0 && r.snapshot_generation == 0;
    }
    if (!pending() || r.phase < record_.phase ||
        static_cast<uint32_t>(r.phase) > static_cast<uint32_t>(record_.phase) + 1) {
        return false;
    }
    if (r.phase == record_.phase) {
        return r.cursor > record_.cursor && r.snapshot_generation == record_.snapshot_generation;
    }
    return r.cursor == 0 && r.effect == PrivacyEffect::none &&
           (r.phase == PrivacyPhase::compact ||
            r.snapshot_generation == record_.snapshot_generation);
}
} // namespace ayni::work
