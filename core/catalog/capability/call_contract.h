// Immutable call semantics, separate from authority domains and live attempts.
// A Provider implementation must offer the exact contract selected by a Binding.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_CALL_CONTRACT_H
#define AYNI_CORE_CATALOG_CAPABILITY_CALL_CONTRACT_H

#include "catalog/catalog_id.h"
#include "catalog/resource/resource_registry.h"
#include "foundation/bounded_array.h"

#include <array>
#include <ayni/calls.h>

namespace ayni::catalog {

enum class CallForm : uint32_t { unary = 1, action, subscription };
enum class CallClass : uint32_t { tool = 1, model };
enum class CallIdempotency : uint32_t { idempotent = 1, not_idempotent };
enum class QueryAuthority : uint32_t { none = 1, best_effort, authoritative };
enum class CancelMode : uint32_t { none = 1, request, confirmed };

// Independent effect flags; neither model classification nor idempotency can
// erase a declared effect or imply permission to retry an unknown operation.
constexpr uint32_t call_effect_egress = AYNI_CALL_EFFECT_EGRESS;
constexpr uint32_t call_effect_billing = AYNI_CALL_EFFECT_BILLING;
constexpr uint32_t call_effect_notification = AYNI_CALL_EFFECT_NOTIFICATION;
constexpr uint32_t call_effect_physical = AYNI_CALL_EFFECT_PHYSICAL;
constexpr uint32_t call_effect_mask =
    call_effect_egress | call_effect_billing | call_effect_notification | call_effect_physical;
constexpr uint32_t call_text_limit_max = AYNI_CALL_TEXT_BYTES_MAX;
constexpr uint32_t call_fields_text_input_format = AYNI_CALL_INPUT_FIELDS_TEXT;
constexpr uint32_t call_text_result_format = AYNI_CALL_RESULT_TEXT;
// Text or typed tool calls; only nonphysical, non-subscription MODEL contracts.
constexpr uint32_t call_tool_calls_result_format = AYNI_CALL_RESULT_TOOL_CALLS;

// One static requirement; binding resolves the name only after registration.
struct CallResource {
    CatalogId id;   // Nonempty exact Resource ID; distinct within a requirement group.
    uint32_t units; // Positive resource units; checked against its catalog at start.
};

// Typed input of complete authority fields + UTF-8 text; a result of UTF-8
// text or, for format 2, text or a tool-call list (ADR-0054). The enclosing
// capability version owns business semantics; these format versions identify
// representation, not ABI or storage schema.
struct CallContract {
    CallForm form;               // Unary/action/subscription; no implicit default.
    CallClass category;          // MODEL always consumes the Task model count.
    uint32_t effects;            // Zero is pure; otherwise only call_effect_mask bits.
    CallIdempotency idempotency; // Independent semantic repeatability declaration.
    QueryAuthority query;        // NONE never interprets an absent record as nonacceptance.
    CancelMode cancel;           // Software cancellation is not physical stop confirmation.
    uint32_t dedupe_window_ms;   // Zero means no service-side operation-ID deduplication.
    uint32_t timeout_ms;         // Positive duration, never a saved monotonic timestamp.
    uint32_t input_format;       // First fields/text representation is 1; unknown rejected.
    uint32_t result_format;      // Text 1 or text/tool calls 2; unknown rejected.
    uint32_t input_text_limit;   // Bytes, 0..call_text_limit_max; zero permits only empty text.
    uint32_t result_text_limit;  // Bytes, also of a tool list; failure does not truncate.
    uint32_t resource_count;     // Valid prefix of resources, 0..resource_group_max.
    std::array<CallResource, resource_group_max> resources; // Copied, sorted by ID.
};

// One exact capability version's behavior, supplementing its authority domain.
struct CallCapability {
    CatalogId id;          // Nonempty capability ID; registered independently of call order.
    uint32_t version;      // Positive business semantic version, matched exactly.
    CallContract contract; // Immutable declaration owned by this catalog entry.
};

// Admission checks all values once and canonicalizes resource order. These
// are internal typed descriptors; ABI pointer/struct-size checks stay at entry.
ayni_status_t normalize_call_contract(const CallContract &input, CallContract &out);
// Compares already normalized contracts, including limits and every resource.
bool same_call_contract(const CallContract &left, const CallContract &right);
// ABI shape and values use the same normalizer as internal typed registration.
// Copies all nested views; any failure preserves out.
ayni_status_t decode_call_capability(const ayni_call_capability_v1 &input, CallCapability &out);

// Bounded static directory. No registered pointer or concrete Provider escapes.
class CallCapabilityRegistry {
  public:
    static constexpr std::size_t storage_alignment = alignof(CallCapability);
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<CallCapability>::storage_size(capacity);
    }
    // Aligned caller arena; null only for zero capacity. No runtime removal.
    CallCapabilityRegistry(void *storage, uint32_t capacity) : entries_(storage, capacity) {}
    // Copies/canonicalizes on success, no mutation for invalid/duplicate/full input.
    ayni_status_t add(const CallCapability &capability);
    // Input was normalized/decoded in this call; no duplicate semantic validation.
    ayni_status_t add_validated(const CallCapability &capability);
    // Exact ID/version lookup. Failure leaves index untouched.
    bool find(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    // Immutable entry at index < size(), valid until registry destruction.
    const CallCapability &at(uint32_t index) const { return entries_[index]; }
    // Occupied entries, not configured capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<CallCapability> entries_; // Sole owner of copied call semantics.
};
} // namespace ayni::catalog
#endif
