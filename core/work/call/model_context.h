// Disclosure evidence is separate from execution authority and private memory.
#ifndef AYNI_CORE_WORK_CALL_MODEL_CONTEXT_H
#define AYNI_CORE_WORK_CALL_MODEL_CONTEXT_H
#include <array>
#include <ayni/directory.h>
namespace ayni::work {
// Call-owned, immutable disclosure ceiling. Policy position meanings are frozen
// by the owning Goal. A restart invalidates the metadata, never action facts.
struct ModelContext {
    uint64_t boot_epoch = 0; // Zero is no metadata dependency; positive is the disclosure boot.
    std::array<uint64_t, AYNI_CAPABILITY_DISCLOSURE_WORDS>
        visible{}; // One bit per policy position.
    bool operator==(const ModelContext &other) const {
        return boot_epoch == other.boot_epoch && visible == other.visible;
    }
};
inline constexpr uint32_t model_context_record_bytes =
    sizeof(uint64_t) * (1U + AYNI_CAPABILITY_DISCLOSURE_WORDS);
} // namespace ayni::work
#endif
