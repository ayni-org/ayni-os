// Provider posting is the permitted cross-thread boundary. Semantic effects,
// schemas and query authority are checked by the owner after the mailbox copy.
#include "catalog/abi_checks.h"
#include "kernel/completion/provider_mailbox.h"

#include <optional>

namespace {
// Do not rely on the internal enum's ordinal values matching the C ABI.
std::optional<ayni::kernel::ProviderMessageKind> kind_of(ayni_call_outcome_t outcome) {
    using Kind = ayni::kernel::ProviderMessageKind;
    switch (outcome) {
    case AYNI_CALL_COMPLETED:
        return Kind::completed;
    case AYNI_CALL_FAILED:
        return Kind::failed;
    case AYNI_CALL_CANCELLED:
        return Kind::cancelled;
    case AYNI_CALL_PROGRESS:
        return Kind::progress;
    case AYNI_CALL_CANNOT_CANCEL:
        return Kind::cannot_cancel;
    case AYNI_CALL_CANCEL_UNKNOWN:
        return Kind::cancel_unknown;
    case AYNI_CALL_ABANDONED:
        return Kind::abandoned;
    default:
        return {};
    }
}
} // namespace

// The target is immutable for the pool lifetime. A reused slot is identified
// only by the producer's original result.attempt, checked under the Host lock.
extern "C" ayni_status_t ayni_provider_post(ayni_provider_completion_t *completion,
                                            const ayni_provider_result_v1 *result) {
    if (completion == nullptr || result == nullptr || completion->pool == nullptr ||
        !completion->pool->owns(completion)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status =
        ayni::catalog::check_struct_size(result->struct_size, sizeof(*result), sizeof(*result));
    if (status != AYNI_OK) {
        return status;
    }
    const auto kind = kind_of(result->outcome);
    if (!kind) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni::kernel::ProviderMessage message{
        *kind,
        result->sequence,
        result->status,
        result->reason,
        {reinterpret_cast<const uint8_t *>(result->result.data), result->result.size},
        result->effect,
        result->query_result};
    return completion->pool->post({completion->index, result->attempt}, message);
}
