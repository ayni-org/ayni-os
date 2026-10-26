// Only authenticated management may inspect external file actions or acknowledge them.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/service/memory/memory_access.h"
#include "kernel/service/memory/privacy_service.h"
#include <ayni/context.h>
extern "C" ayni_status_t ayni_get_context(const ayni_kernel_t *kernel, uint64_t session,
                                          uint64_t owner, ayni_context_info_v1 *out) {
    if (kernel == nullptr || out == nullptr || owner == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto gate = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
    if (gate != AYNI_OK) {
        return gate;
    }
    const auto &model = kernel->model();
    if (!ayni::kernel::memory_management(model, session, kernel->durable_seq())) {
        return AYNI_DENIED;
    }
    uint32_t index = 0;
    if (!model.contexts.find_owner(owner, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &row = model.contexts.at(index);
    *out = {sizeof(*out),
            row.revision,
            row.id,
            row.owner_instance_id,
            row.generation,
            row.last_turn,
            static_cast<ayni_context_action_t>(row.pending.action),
            row.pending.old_revision,
            row.pending.turn_id,
            row.pending.task_id,
            row.stamp.commit_seq,
            ayni::kernel::context_current({model, kernel->now_us(), kernel->durable_seq()}, index)
                ? 1U
                : 0U,
            0};
    return AYNI_OK;
}

extern "C" ayni_status_t ayni_get_privacy_status(const ayni_kernel_t *kernel, uint64_t session,
                                                 ayni_privacy_info_v1 *out) {
    if (kernel == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto gate = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
    if (gate != AYNI_OK) {
        return gate;
    }
    const auto &model = kernel->model();
    if (!ayni::kernel::memory_management(model, session, kernel->durable_seq())) {
        return AYNI_DENIED;
    }
    const auto &row = model.privacy.record();
    using ayni::work::PrivacyPhase;
    const auto phase =
        row.phase == PrivacyPhase::idle ? AYNI_PRIVACY_IDLE
        : row.phase == PrivacyPhase::complete && row.stamp.commit_seq <= kernel->durable_seq()
            ? AYNI_PRIVACY_COMPLETE
        : row.phase >= PrivacyPhase::compact ? AYNI_PRIVACY_COMPACTING
        : row.phase == PrivacyPhase::files   ? AYNI_PRIVACY_FILES
                                             : AYNI_PRIVACY_ERASING;
    *out = {sizeof(*out),
            phase,
            row.request_seq,
            row.stamp.commit_seq,
            ayni::kernel::privacy_progress_status(model, kernel->snapshot_generation()),
            0};
    return AYNI_OK;
}
