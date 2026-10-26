#include "kernel/service/intent/context_intent.h"
#include "kernel/service/memory/context_service.h"
namespace ayni::kernel {
namespace {
ayni_status_t open_context(WorkModel &model, const ContextCommand &command,
                           IntentOutcome &outcome) {
    if (model.contexts.capacity() == 0) {
        return AYNI_UNSUPPORTED;
    }
    if (model.agents.find_instance(command.owner_instance_id) == nullptr) {
        return AYNI_NOT_FOUND;
    }
    uint32_t index = 0;
    if (model.contexts.find_owner(command.owner_instance_id, index)) {
        return AYNI_INVALID_STATE;
    }
    if (model.contexts.size() == model.contexts.capacity() || !model.ids.available()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint64_t id = 0;
    if (!model.ids.allocate(id)) {
        return AYNI_INTERNAL;
    }
    const auto stamp = model.transitions.next();
    model.contexts.create(id, command.owner_instance_id, stamp);
    outcome.object = {0, id, 1, AYNI_OBJECT_CONTEXT};
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}
// Stage the exact completed Task union before touching the anchor or command result.
ayni_status_t publication_sources(const MemoryAccess &access, const ContextCommand &command,
                                  uint32_t index, knowledge::MemoryDependencies &out) {
    const auto &context = access.model.contexts.at(index);
    if (context.pending.action != knowledge::ContextAction::none) {
        return AYNI_BUSY;
    }
    if (context.generation == UINT64_MAX || context.last_turn == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (command.turn_id != context.last_turn + 1) {
        return AYNI_REVISION_CONFLICT;
    }
    uint32_t task = 0;
    if (!access.model.tasks.find(command.task_id, task)) {
        return AYNI_NOT_FOUND;
    }
    if (access.model.tasks.at(task).commit_seq > access.durable_seq) {
        return AYNI_BUSY;
    }
    if (!task_content_current(access, task) ||
        !task_context_sources(access.model, task, context, out)) {
        return AYNI_EXPIRED;
    }
    return AYNI_OK;
}
} // namespace
// A deletion may supersede an unfinished publication. Another deletion cannot change the exact
// pending target before the outer store acknowledges it, making normal restart deterministic.
ayni_status_t apply_context_intent(WorkModel &model, const ContextCommand &command, uint64_t now_us,
                                   uint64_t durable_seq, IntentOutcome &outcome) {
    using knowledge::ContextAction;
    if (command.kind == AYNI_INTENT_CONTEXT_OPEN) {
        return open_context(model, command, outcome);
    }
    uint32_t index = 0;
    if (!model.contexts.find(command.context_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &old = model.contexts.at(index);
    if (old.revision != command.expected_revision || old.generation != command.generation) {
        return AYNI_REVISION_CONFLICT;
    }
    if (old.stamp.commit_seq > durable_seq) {
        return AYNI_BUSY;
    }
    knowledge::MemoryDependencies sources{};
    if (command.kind == AYNI_INTENT_CONTEXT_PUBLISH) {
        const auto status =
            publication_sources({model, now_us, durable_seq}, command, index, sources);
        if (status != AYNI_OK) {
            return status;
        }
    } else if (command.kind == AYNI_INTENT_CONTEXT_FORGET) {
        if (old.pending.action > ContextAction::publish) {
            return AYNI_BUSY;
        }
        if (old.revision == UINT32_MAX || old.generation == UINT64_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        if (command.action == ContextAction::erase_round && command.turn_id > old.last_turn) {
            return AYNI_NOT_FOUND;
        }
    } else {
        if (old.pending.action == ContextAction::none) {
            return AYNI_INVALID_STATE;
        }
        if (old.pending.action == ContextAction::publish &&
            !context_current({model, now_us, durable_seq}, index)) {
            return AYNI_EXPIRED;
        }
    }
    const auto stamp = model.transitions.next();
    if (command.kind == AYNI_INTENT_CONTEXT_PUBLISH) {
        model.release_content_references(old.dependencies, stamp.commit_seq);
        model.contexts.publish(index, command.task_id, sources, stamp);
    } else if (command.kind == AYNI_INTENT_CONTEXT_FORGET) {
        model.release_content_references(old.dependencies, stamp.commit_seq);
        model.contexts.erase(index, command.action, command.turn_id, stamp);
        model.privacy.request(stamp);
        outcome.due_work = true;
    } else {
        model.contexts.acknowledge(index, stamp);
    }
    outcome.object = {0, command.context_id, model.contexts.at(index).revision,
                      AYNI_OBJECT_CONTEXT};
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}
} // namespace ayni::kernel
