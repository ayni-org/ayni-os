// Which transition last changed an object (contracts/domain/primitives.md):
// transition_id names the transition and commit_seq places it in the log, so
// the change is durable once durable_seq reaches it (C-COMMIT-004).

#ifndef AYNI_CORE_FOUNDATION_TRANSITION_STAMP_H
#define AYNI_CORE_FOUNDATION_TRANSITION_STAMP_H

#include <cstdint>

namespace ayni::foundation {

struct TransitionStamp {
    uint64_t transition_id;
    uint64_t commit_seq;
};

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_TRANSITION_STAMP_H
