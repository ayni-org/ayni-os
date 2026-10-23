#include "kernel/kernel_state.h"

#include "kernel/intent/authority_intent.h"

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
