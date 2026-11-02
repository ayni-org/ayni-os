// Pure body reference checks shared by every body reader (ADR-0096).
#include "foundation/digest.h"

#include <ayni/bodies.h>

extern "C" uint64_t ayni_body_digest(ayni_bytes_v1 bytes) {
    ayni::foundation::Digest digest;
    if (bytes.data != nullptr) {
        digest.add(bytes.data, bytes.size);
    }
    return digest.value();
}

extern "C" ayni_status_t ayni_body_verify(const ayni_body_ref_v1 *ref, ayni_bytes_v1 bytes) {
    if (ref == nullptr || ref->id == 0U || ref->reserved != 0U ||
        (bytes.size != 0U && bytes.data == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return ref->size == bytes.size && ref->digest == ayni_body_digest(bytes) ? AYNI_OK
                                                                             : AYNI_DENIED;
}
