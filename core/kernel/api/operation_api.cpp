// ADR-0115: scalar management view of retired operations; no Task or private body lookup.
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include <cstring>

ayni_status_t ayni_get_unresolved_op(const ayni_kernel_t *kernel, uint64_t call_id,
                                     ayni_unresolved_op_v1 *out) {
    if (!kernel || !out || out->struct_size != sizeof(*out) || call_id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
    if (gated != AYNI_OK) {
        return gated;
    }
    const auto &ledger = kernel->model().unresolved_ops;
    uint32_t index = 0;
    if (!ledger.find(call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto row = ledger.record(index);
    const auto &state = row.state;
    ayni_unresolved_op_v1 info{};
    info.struct_size = sizeof(info);
    info.revision = state.revision;
    info.logical_call_id = row.identity.call_id;
    info.operation_id = row.identity.operation_id;
    info.task_id = row.identity.task_id;
    info.owner_instance_id = row.identity.owner_instance_id;
    info.attempt = state.attempt;
    info.archived_boot_epoch = state.archived.boot_epoch;
    info.archived_transition_id = state.archived.transition.transition_id;
    info.archived_commit_seq = state.archived.transition.commit_seq;
    info.requires_seq = state.latest.transition.commit_seq;
    info.outcome = state.outcome;
    info.status = state.status;
    info.reason = state.reason;
    info.effect = static_cast<ayni_call_effect_t>(row.action.effect);
    if (state.query) {
        info.query_phase = static_cast<ayni_call_query_phase_t>(state.query->phase);
        info.query_status = state.query->status;
        info.query_reason = state.query->reason;
        info.query_conclusion = state.query->conclusion;
        info.query_attempt = state.query->attempt;
    }
    info.binding_version = row.identity.binding_version;
    info.binding_size = row.identity.binding_id.size();
    std::memcpy(info.binding_id, row.identity.binding_id.data(), info.binding_size);
    if (state.execution_stop) {
        info.execution_phase = state.execution_stop->phase;
        info.stop_cause = state.execution_stop->stop_cause;
        info.target = state.execution_stop->target;
    }
    *out = info;
    return AYNI_OK;
}
