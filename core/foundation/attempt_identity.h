// Host and Provider delivery share the exact attempt identity rule (C-ID-006).
#ifndef AYNI_CORE_FOUNDATION_ATTEMPT_IDENTITY_H
#define AYNI_CORE_FOUNDATION_ATTEMPT_IDENTITY_H

#include <ayni/ayni.h>

namespace ayni::foundation {
// Neither boot nor request alone identifies an asynchronous result.
inline bool same_attempt(ayni_attempt_ref_v1 a, ayni_attempt_ref_v1 b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}
} // namespace ayni::foundation
#endif
