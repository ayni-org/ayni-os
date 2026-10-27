// Owner-thread copy boundary; work owns calendar rules and cursor semantics.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"

extern "C" ayni_status_t ayni_get_calendar(const ayni_kernel_t *kernel, uint64_t goal_id,
                                           ayni_calendar_info_v1 *out) {
    if (kernel == nullptr || out == nullptr || goal_id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
    if (gated != AYNI_OK) {
        return gated;
    }
    const auto &model = kernel->model();
    uint32_t index = 0;
    if (!model.goals.find(goal_id, index) || model.calendars.at(index).goal_id != goal_id) {
        return AYNI_NOT_FOUND;
    }
    const auto &row = model.calendars.at(index);
    const auto clock = kernel->calendar_clock(kernel->now_us());
    ayni_calendar_info_v1 info{};
    info.struct_size = sizeof(info);
    info.waiting_for_time = clock.trusted ? 0 : 1;
    info.goal_id = goal_id;
    info.rule = ayni::work::calendar_rule_info(row.rule);
    info.last_local_s = row.last_local_s;
    info.last_utc_s = row.last_utc_s;
    info.next_utc_s = ayni::work::calendar_next(row.rule, row.last_local_s);
    info.taken = row.taken;
    info.transition_id = row.stamp.transition_id;
    info.commit_seq = row.stamp.commit_seq;
    *out = info;
    return AYNI_OK;
}
