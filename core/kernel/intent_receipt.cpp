#include "kernel/kernel_state.h"

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
    if (pending != nullptr && pending->queued_commit_seq == 0U) {
        receipt.phase = AYNI_INTENT_PENDING;
        receipt.effect = AYNI_INTENT_EFFECT_NONE;
        receipt.object = {0, pending->accepted.target_grant_id, 0, AYNI_OBJECT_GRANT};
    } else {
        if (mapping == nullptr) {
            return AYNI_INTERNAL;
        }
        receipt.object = mapping->object;
        receipt.effect = mapping->effect;
        receipt.commit_seq = mapping->commit_seq;
        const bool durable = log_started_ && mapping->commit_seq <= log.durable_seq();
        receipt.phase = durable ? AYNI_INTENT_DURABLE : AYNI_INTENT_APPLIED;
        receipt.applied_boot_epoch = durable ? 0U : log.boot_epoch();
    }
    out = receipt;
    return AYNI_OK;
}
