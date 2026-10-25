// The kernel's side of intents: the transition ayni_submit_intent takes,
// with the sample of the kernel its rules read, and the receipt a command
// reports afterwards (ADR-0017, ADR-0031).

#include "kernel/kernel_state.h"

#include "kernel/intent/authority_intent.h"

namespace {

ayni::work::InputResumeBoundary sample_input_boundary(void *context) {
    return static_cast<ayni::kernel::EventIngress *>(context)->resume_boundary();
}

} // namespace

ayni_status_t ayni_kernel::submit_intent(const ayni::kernel::Intent &intent,
                                         ayni::kernel::IntentOutcome &outcome,
                                         ayni::kernel::IntentFailure &failure,
                                         bool &out_committed) {
    ayni_status_t status = AYNI_OK;
    out_committed = run_transition([&] {
        const uint64_t before = work.transitions.next_commit_seq();
        const ayni::kernel::IntentContext context{host.monotonic_now_us(host.ctx),
                                                  log.durable_seq(),
                                                  storage_has_room(),
                                                  storage_admission_status(),
                                                  {host.ctx, host.monotonic_now_us},
                                                  {&ingress, sample_input_boundary}};
        status = ayni::kernel::apply_intent(work, intent, context, outcome, failure);
        // A revocation takes no transition of its own (ADR-0031).
        if (status == AYNI_OK && intent.kind == AYNI_INTENT_GRANT_REVOKE && !outcome.retried &&
            storage_has_room() && storage_admission_status() == AYNI_OK &&
            work.transitions.next_commit_seq() == before) {
            static_cast<void>(ayni::kernel::apply_pending_revoke(work));
        }
        // Also after AYNI_INTERNAL, which used up a commit_seq (ayni.h).
        return ayni::kernel::ItemEnd{work.transitions.next_commit_seq() != before
                                         ? ayni::kernel::ItemEnd::Kind::applied
                                         : ayni::kernel::ItemEnd::Kind::none};
    });
    return status;
}

// C-AUTH-027: report one accepted result, with durability from the active
// writer only. Partial recovery exposes accepted pending controls, not its window.
ayni_status_t ayni_kernel::intent_receipt(uint64_t principal_id, uint64_t command_id,
                                          ayni_intent_receipt_v1 &out) const {
    if (principal_id == 0U || command_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto *owner = work.principals.owner();
    if (owner == nullptr || owner->principal_id != principal_id) {
        return AYNI_DENIED;
    }
    const ayni::kernel::PendingRevoke *pending = nullptr;
    const ayni::kernel::CommandMapping *mapping = nullptr;
    const auto lookup = work.commands.lookup_receipt(principal_id, command_id, pending, mapping);
    // A partial replay must not expose an incomplete ordinary retry window.
    if (pending == nullptr && !log_started_) {
        return AYNI_NOT_READY;
    }
    if (lookup != ayni::kernel::CommandTable::Lookup::retried) {
        return lookup == ayni::kernel::CommandTable::Lookup::expired ? AYNI_EXPIRED
                                                                     : AYNI_NOT_FOUND;
    }
    ayni_intent_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.principal_id = principal_id;
    receipt.command_id = command_id;
    if (pending != nullptr) {
        if (pending->queued_commit_seq != 0U && mapping == nullptr) {
            return AYNI_INTERNAL;
        }
        ayni::kernel::IntentOutcome outcome{};
        ayni::kernel::pending_revoke_outcome(*pending, mapping, outcome);
        receipt.object = outcome.object;
        receipt.commit_seq = outcome.commit_seq;
        receipt.phase = outcome.phase;
        receipt.effect = outcome.effect;
        receipt.applied_boot_epoch = outcome.applied_boot_epoch;
    } else {
        if (mapping == nullptr) {
            return AYNI_INTERNAL;
        }
        receipt.object = mapping->object;
        receipt.effect = mapping->effect;
        receipt.commit_seq = mapping->commit_seq;
        receipt.phase = AYNI_INTENT_APPLIED;
        receipt.applied_boot_epoch = log.boot_epoch();
    }
    // ADR-0031: reload clears queued stamps before restoring the window;
    // log_started_ publishes only its completed/reconciled boot. Therefore an
    // APPLIED pending control here belongs to that writer, unlike a raw floor.
    if (receipt.phase == AYNI_INTENT_APPLIED && log_started_ &&
        receipt.commit_seq <= log.durable_seq()) {
        receipt.phase = AYNI_INTENT_DURABLE;
        receipt.applied_boot_epoch = 0;
    }
    out = receipt;
    return AYNI_OK;
}
