// Provider posting is the permitted cross-thread boundary. Semantic effects,
// schemas and query authority are checked by the owner after the mailbox copy.
#include "catalog/abi_checks.h"
#include "kernel/ports/provider_mailbox.h"
#include "work/call/tool_calls.h"

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

// The list as measured: its address and count are not read from the caller again.
struct MeasuredCalls {
    const ayni_tool_call_v1 *calls;
    uint32_t count;
};

// Runs under the mailbox lock, during the caller's post.
void write_calls(const void *context, uint8_t *out, uint32_t size) {
    const auto &list = *static_cast<const MeasuredCalls *>(context);
    ayni::work::write_tool_calls(list.calls, list.count, out, size);
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
    if (!kind || result->tool_reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::kernel::ProviderMessage message{
        *kind,
        result->sequence,
        result->status,
        result->reason,
        {reinterpret_cast<const uint8_t *>(result->result.data), result->result.size},
        result->effect,
        result->query_result};
    // Shape is checked on this thread; whether each tool was offered and its
    // values allowed is checked by the owner against its catalogs (ADR-0054).
    const MeasuredCalls measured_calls{result->tool_calls, result->tool_call_count};
    const ayni::kernel::PayloadWriter writer{&measured_calls, write_calls};
    if (result->tool_call_count != 0U) {
        uint32_t size = 0;
        if (result->outcome != AYNI_CALL_COMPLETED || result->result.size != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto measured =
            ayni::work::measure_tool_calls(result->tool_calls, result->tool_call_count, size);
        if (measured != AYNI_OK) {
            return measured;
        }
        message.payload = {nullptr, size};
        message.body = ayni::work::CallResultKind::tool_calls;
        message.writer = &writer;
    }
    return completion->pool->post({completion->index, result->attempt}, message);
}
