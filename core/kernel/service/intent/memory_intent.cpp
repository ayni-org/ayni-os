#include "kernel/service/intent/memory_intent.h"
#include "kernel/service/memory/memory_access.h"
namespace ayni::kernel {
namespace {
ayni_status_t save_memory(WorkModel &model, const MemoryCommand &command, IntentOutcome &outcome) {
    const auto &value = command.value;
    if (!memory_namespace_exists(model, value.space, value.namespace_id)) {
        return AYNI_NOT_FOUND;
    }
    knowledge::MemoryWritePlan plan{};
    const auto status =
        model.memories.check_write(command.target_id, command.expected_revision, value, plan);
    if (status != AYNI_OK) {
        return status;
    }
    if (command.target_id == 0 && !model.ids.available()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint64_t id = command.target_id;
    if (id == 0 && !model.ids.allocate(id)) {
        return AYNI_INTERNAL;
    }
    const auto stamp = model.transitions.next();
    if (command.target_id != 0) {
        model.release_content_references(model.memories.at(plan.index).dependencies,
                                         stamp.commit_seq);
    }
    model.memories.install(plan, id, value, stamp);
    outcome.object = {0, id, plan.revision, AYNI_OBJECT_MEMORY};
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}
} // namespace
// Confirmation changes trust only. Deletion closes use before a later bounded scrub transition.
ayni_status_t apply_memory_intent(WorkModel &model, const MemoryCommand &command, uint64_t now_us,
                                  uint64_t durable_seq, IntentOutcome &outcome) {
    if (command.kind == AYNI_INTENT_MEMORY_SAVE) {
        return save_memory(model, command, outcome);
    }
    uint32_t index = 0;
    const auto status =
        model.memories.check_live(command.target_id, command.expected_revision, index);
    if (status != AYNI_OK) {
        return status;
    }
    // Explicit save is already confirmed; only an unconfirmed inferred/derived fact changes trust.
    if (command.kind == AYNI_INTENT_MEMORY_CONFIRM && model.memories.at(index).confirmed) {
        return AYNI_INVALID_STATE;
    }
    if (command.kind == AYNI_INTENT_MEMORY_CONFIRM &&
        !memory_current({model, now_us, durable_seq}, index)) {
        return AYNI_EXPIRED;
    }
    const auto stamp = model.transitions.next();
    if (command.kind == AYNI_INTENT_MEMORY_CONFIRM) {
        model.memories.confirm(index, stamp);
    } else {
        model.memories.tombstone(index, stamp);
        outcome.due_work = true;
    }
    outcome.object = {0, command.target_id, model.memories.at(index).revision, AYNI_OBJECT_MEMORY};
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}
} // namespace ayni::kernel
