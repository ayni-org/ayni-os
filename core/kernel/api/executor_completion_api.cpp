// Executor answers cross from producer threads to the owner, like Provider
// posts (ADR-0052).
// Whether a nonce or status is valid is checked by the owner after the copy.
#include "catalog/abi_checks.h"
#include "kernel/ports/executor_mailbox.h"

extern "C" ayni_status_t ayni_executor_post(ayni_executor_completion_t *completion,
                                            const ayni_executor_response_v1 *response) {
    if (completion == nullptr || response == nullptr || completion->mailbox == nullptr ||
        !completion->mailbox->owns(completion)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = ayni::catalog::check_struct_size(response->struct_size, sizeof(*response),
                                                         sizeof(*response));
    if (status != AYNI_OK) {
        return status;
    }
    return completion->mailbox->post(*completion, *response);
}
