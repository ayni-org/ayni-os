// Durable subscription intent and confirmed generation facts. No clocks or I/O handles.
#ifndef AYNI_CORE_WORK_SUBSCRIPTION_FACT_H
#define AYNI_CORE_WORK_SUBSCRIPTION_FACT_H
#include <ayni/subscriptions.h>
#include <cstdint>
namespace ayni::work {
// Same meanings as the public phases, mapped deliberately at the ABI boundary.
enum class SubscriptionPhase : uint32_t {
    starting = 1,
    active,
    stopping,
    stopped,
    expired,
    failed
};
// Pause/shutdown closes one generation while preserving the Task's collection intent.
enum class SubscriptionDesired : uint32_t { open = 1, closed };
// Only opening admission blocks the Task; a started/preserved subscription is independent.
enum class SubscriptionAttachment : uint32_t { foreground = 1, background };
// Owned with its logical Call row; original Task/Agent/source live in that row's identity/contract.
struct SubscriptionFact {
    SubscriptionPhase phase = SubscriptionPhase::starting; // Explicit generation state.
    SubscriptionDesired desired =
        SubscriptionDesired::open; // Closed only by explicit close/Task end.
    SubscriptionAttachment attachment = SubscriptionAttachment::foreground; // Task call link rule.
    uint64_t generation =
        1; // Globally unique Kernel ID; changes only after known stop/borrow return.
    ayni_status_t control_status = AYNI_OK;          // OK until a classified start/renew failure.
    ayni_reason_t control_reason = AYNI_REASON_NONE; // NONE except malformed control evidence.
    uint64_t lease_sequence = 0; // Highest confirmed control this opening, 0 before STARTED.
};
// Shared intrinsic checks for live creation and record decode.
bool valid_subscription(const SubscriptionFact &fact);
// Same generation advances phases/ACKs; a new generation requires confirmed closure.
bool subscription_advances(const SubscriptionFact &before, const SubscriptionFact &after);
} // namespace ayni::work
#endif
