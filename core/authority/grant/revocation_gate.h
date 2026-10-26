// Owns accepted volatile denies independently of retained Grant facts (C-AUTH-027).
// Reload/replacement cannot reopen accepted revocations; only explicit handoff or teardown clears.
#ifndef AYNI_CORE_AUTHORITY_GRANT_REVOCATION_GATE_H
#define AYNI_CORE_AUTHORITY_GRANT_REVOCATION_GATE_H
#include "foundation/bounded_array.h"
#include <cstddef>
#include <cstdint>
namespace ayni::authority {
class RevocationGate {
  public:
    // One uint64_t slot per concurrently accepted revocation, no heap allocation.
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<uint64_t>::storage_size(capacity);
    }
    // Borrow aligned caller storage until this gate is destroyed; empty initially.
    RevocationGate(void *storage, uint32_t capacity) : ids_(storage, capacity) {}
    // True for a valid ID with room, or an already accepted deny.
    bool can_gate(uint64_t grant_id) const;
    // Idempotently install an exact deny; no Grant facts are needed or changed.
    bool install_gate(uint64_t grant_id);
    // Release only this ID after durable handoff; false when absent.
    bool release_gate(uint64_t grant_id);
    // Query accepted control, even if the corresponding Grant is absent after reload.
    bool gated(uint64_t grant_id) const;
    // Number of retained denies, bounded by the construction capacity.
    uint32_t count() const { return ids_.size(); }
    // Full owner teardown only, never restoration or table-slot replacement.
    void clear() { ids_.clear(); }

  private:
    foundation::BoundedArray<uint64_t>
        ids_; // Nonzero Kernel IDs; no duplicates, no Grant-row indices.
};
} // namespace ayni::authority
#endif
