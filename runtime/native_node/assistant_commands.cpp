#include "assistant_state.h"
#include <algorithm>
#include <chrono>
#include <cstring>
namespace ayni::runtime {
namespace {
constexpr auto local_io_timeout = std::chrono::seconds(10);
Reply failure(ayni_status_t status) {
    Reply reply;
    reply.status = status;
    return reply;
}
} // namespace
Reply Assistant::Impl::submit(ayni_intent_kind_t kind, const void *body, uint32_t revision) {
    const auto collected = collect_receipts();
    if (collected != AYNI_OK) {
        return failure(collected);
    }
    if (pending_count == pending_commands.size()) {
        return failure(AYNI_BUSY);
    }
    const auto command_id = command_ids.next();
    if (!command_id) {
        return failure(AYNI_CAPACITY_EXCEEDED);
    }
    const ayni_intent_v1 intent{sizeof(intent), kind, command_id, session, revision, 0, body};
    ayni_intent_result_v1 result{};
    result.struct_size = sizeof(result);
    ayni_error_v1 error{};
    error.struct_size = sizeof(error);
    Reply reply;
    reply.status = ayni_submit_intent(node.kernel(), &intent, &result, &error);
    reply.reason = error.reason;
    if (reply.status == AYNI_OK) {
        reply.id = result.object.id;
        reply.commit_seq = result.commit_seq;
        reply.command_id = intent.command_id;
        reply.phase = result.phase;
        if (result.phase != AYNI_INTENT_DURABLE) {
            pending_commands[pending_count++] = intent.command_id;
        }
        reply.message = Message::accepted;
    }
    return reply;
}
// PENDING revocations have commit_seq=0. Poll actual receipts before every new
// mutation so durable entries cannot be evicted before we observe completion.
ayni_status_t Assistant::Impl::collect_receipts() {
    for (uint32_t at = 0; at < pending_count;) {
        ayni_intent_receipt_v1 receipt{};
        receipt.struct_size = sizeof(receipt);
        const auto status =
            ayni_get_intent_receipt(node.kernel(), demo_owner, pending_commands[at], &receipt);
        if (status != AYNI_OK) {
            return status;
        }
        if (receipt.phase == AYNI_INTENT_DURABLE) {
            pending_commands[at] = pending_commands[--pending_count];
        } else {
            ++at;
        }
    }
    return AYNI_OK;
}
// One model turn of the single assistant (ADR-0072): no routing, one pending turn at a time.
Reply Assistant::Impl::say(const Command &command) {
    auto status = conversation.begin(runtime_text(command.text), command.id);
    if (status != AYNI_OK) {
        return failure(status);
    }
    // The CLI returns the actual Core Goal identity, after bounded local admission only.
    // Network/model execution remains asynchronous; no old action is re-created.
    const auto end = std::chrono::steady_clock::now() + local_io_timeout;
    while (!conversation.latest_goal()) {
        status = advance_kernel();
        if (status == AYNI_OK) {
            status = conversation.poll();
        }
        if (status != AYNI_OK && status != AYNI_BUSY) {
            return failure(status);
        }
        if (const auto *result = conversation.pending_result()) {
            if (!result->goal && result->status != AYNI_OK) {
                auto reply = failure(result->status);
                reply.reason = result->reason;
                return reply;
            }
        }
        if (std::chrono::steady_clock::now() >= end) {
            conversation.cancel();
            return failure(AYNI_BUSY);
        }
        node.wait_for_wake(std::chrono::milliseconds(1));
    }
    auto goal = initialized<ayni_goal_info_v1>();
    status = ayni_get_goal(node.kernel(), conversation.latest_goal(), &goal);
    Reply reply;
    reply.status = status;
    reply.id = goal.goal_id;
    reply.commit_seq = goal.commit_seq;
    reply.phase = AYNI_INTENT_APPLIED;
    reply.message = Message::accepted;
    return reply;
}
// Cancels the turn Goal still in flight. A turn already ending, or none, needs nothing.
Reply Assistant::Impl::cancel_pending_turn() {
    if (!conversation.active() || conversation.record()) {
        return {};
    }
    return failure(conversation.cancel());
}
// Deterministic stop (ADR-0072): no model call and no actuator; the hardware emergency stop
// is a separate chain. Cancels the pending turn, then pauses autonomous work while reminders
// stay. A step already in effect is skipped, so repeating a stop after a partial failure
// finishes the rest and never spends another revision.
Reply Assistant::Impl::stop_work() {
    const auto cancelled = cancel_pending_turn();
    if (cancelled.status != AYNI_OK) {
        return cancelled;
    }
    ayni_pause_group_info_v1 group{};
    group.struct_size = sizeof(group);
    const auto status =
        ayni_get_pause_group(node.kernel(), applications::assistant::autonomous_group, &group);
    if (status != AYNI_OK) {
        return failure(status);
    }
    if (!group.paused) {
        const auto paused = set_autonomy(group, true);
        if (paused.status != AYNI_OK) {
            return paused;
        }
    }
    Reply reply;
    reply.message = Message::stopped;
    return reply;
}
// The autonomous group holds checks, organizing and observing; reminders never join it.
Reply Assistant::Impl::set_autonomy(const ayni_pause_group_info_v1 &group, bool paused) {
    const ayni_pause_group_set_v1 body{sizeof(body), applications::assistant::autonomous_group,
                                       paused ? 1U : 0U, 0};
    return submit(AYNI_INTENT_PAUSE_GROUP_SET, &body, group.revision);
}
// Schedule tool admission (ADR-0072) is the same management submission as an owner command,
// so Core applies the same authority and capacity checks. Shutdown closes it for good.
ayni_status_t Assistant::Impl::propose(void *context, const ayni_goal_create_v1 *goal,
                                       uint64_t *out) {
    auto &self = *static_cast<Impl *>(context);
    if (!goal || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!self.opened || self.shutting_down) {
        return AYNI_INVALID_STATE;
    }
    if (self.proposal_count == self.proposals.size()) {
        return AYNI_BUSY;
    }
    const auto reply = self.submit(AYNI_INTENT_GOAL_CREATE, goal);
    if (reply.status == AYNI_OK) {
        self.proposals[self.proposal_count++] = reply.id;
        *out = reply.id;
    }
    return reply.status;
}
// The creation echo is read through public APIs, with the same immutable parameters used for
// approval, reauthorization and owner announcements. Nothing reconstructs Core records.
ayni_status_t Assistant::Impl::creation(uint64_t id, Creation &out) const {
    out.info.struct_size = sizeof(out.info);
    auto status = ayni_get_goal(node.kernel(), id, &out.info);
    if (status == AYNI_OK) {
        status = ayni_get_goal_parameters(node.kernel(), id, out.parameters.bytes.data(),
                                          strategies::encoded_bytes_max, &out.parameters.size);
    }
    if (status != AYNI_OK) {
        return status;
    }
    const auto &info = out.info;
    auto &original = out.body;
    original.struct_size = sizeof(original);
    original.owner_instance_id = info.owner_instance_id;
    original.parent_goal_id = info.parent_goal_id;
    original.pause_group = info.pause_group;
    original.goal_type_id = {info.goal_type_id, info.goal_type_id_size};
    original.goal_type_version = info.goal_type_version;
    original.parameters = out.parameters.view();
    original.trigger = info.trigger;
    if (info.trigger == AYNI_TRIGGER_CALENDAR_ONCE || info.trigger == AYNI_TRIGGER_CALENDAR_DAILY ||
        info.trigger == AYNI_TRIGGER_CALENDAR_WEEKLY) {
        out.calendar.struct_size = sizeof(out.calendar);
        status = ayni_get_calendar(node.kernel(), id, &out.calendar);
        if (status != AYNI_OK) {
            return status;
        }
        original.calendar = &out.calendar.rule;
    }
    original.trigger_delay_us = info.trigger_delay_us;
    original.trigger_period_us = info.trigger_period_us;
    original.expiry_us = info.original_expiry_us;
    original.trigger_source = {info.trigger_source, info.trigger_source_size};
    return AYNI_OK;
}
Reply Assistant::Impl::approve(uint64_t id, Operation operation) {
    Creation goal;
    auto status = creation(id, goal);
    ayni_goal_authority_info_v1 authority{};
    authority.struct_size = sizeof(authority);
    if (status == AYNI_OK) {
        status = ayni_get_goal_authority(node.kernel(), id, &authority);
    }
    if (status != AYNI_OK) {
        return failure(status);
    }
    if (operation == Operation::reauthorize) {
        const ayni_goal_reauthorize_v1 body{sizeof(body), 0, id, authority.commit_seq, goal.body};
        return submit(AYNI_INTENT_GOAL_REAUTHORIZE, &body, goal.info.revision);
    }
    ayni_goal_approve_v1 body{};
    body.struct_size = sizeof(body);
    body.goal_id = id;
    body.lifetime = AYNI_GRANT_PERMANENT;
    body.original = goal.body;
    for (uint32_t at = 0; at < authority.requirement_count; ++at) {
        ayni_goal_requirement_info_v1 requirement{};
        requirement.struct_size = sizeof(requirement);
        status = ayni_get_goal_requirement(node.kernel(), id, at, &requirement);
        if (status != AYNI_OK) {
            return failure(status);
        }
        if (!requirement.grant_id) {
            body.missing_mask |= uint32_t{1} << at;
        }
    }
    return submit(AYNI_INTENT_GOAL_APPROVE, &body, goal.info.revision);
}
Reply Assistant::Impl::manage(const Command &command) {
    switch (command.operation) {
    case Operation::memory_list:
    case Operation::memory_show:
    case Operation::memory_save:
    case Operation::memory_edit:
    case Operation::memory_confirm:
    case Operation::memory_delete:
    case Operation::memory_share:
    case Operation::memory_export:
    case Operation::profile_list:
    case Operation::profile_save:
        return memory(command);
    case Operation::say:
        return say(command);
    case Operation::schedule:
    case Operation::memory_organize:
        return application_goal(command);
    case Operation::pause_assistant:
    case Operation::resume_assistant: {
        ayni_pause_group_info_v1 info{};
        info.struct_size = sizeof(info);
        const auto status =
            ayni_get_pause_group(node.kernel(), applications::assistant::autonomous_group, &info);
        return status == AYNI_OK
                   ? set_autonomy(info, command.operation == Operation::pause_assistant)
                   : failure(status);
    }
    case Operation::grant: {
        const auto cap = profile.capability(command.text);
        if (!cap.size) {
            return failure(AYNI_INVALID_ARGUMENT);
        }
        const ayni_grant_create_v1 body{sizeof(body),
                                        1,
                                        cap,
                                        assistant(),
                                        0,
                                        AYNI_GRANT_PERMANENT,
                                        0,
                                        0,
                                        profile.scope(command.text)};
        return submit(AYNI_INTENT_GRANT_CREATE, &body);
    }
    case Operation::revoke: {
        ayni_grant_info_v1 grant{};
        grant.struct_size = sizeof(grant);
        const auto status = ayni_get_grant(node.kernel(), command.id, &grant);
        if (status != AYNI_OK) {
            return failure(status);
        }
        const ayni_grant_revoke_v1 body{sizeof(body), 0, command.id};
        return submit(AYNI_INTENT_GRANT_REVOKE, &body, grant.revision);
    }
    case Operation::approve:
    case Operation::reauthorize:
        return approve(command.id, command.operation);
    case Operation::query_result: {
        ayni_call_info_v1 call{};
        call.struct_size = sizeof(call);
        const auto status = ayni_get_call(node.kernel(), command.id, &call);
        if (status != AYNI_OK) {
            return failure(status);
        }
        const ayni_call_query_result_v1 body{sizeof(body), 0, command.id};
        return submit(AYNI_INTENT_CALL_QUERY_RESULT, &body, call.query_revision);
    }
    case Operation::raise_limit:
    case Operation::resume_wait: {
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        const auto status = ayni_get_task(node.kernel(), command.id, &task);
        if (status != AYNI_OK) {
            return failure(status);
        }
        if (command.operation == Operation::raise_limit) {
            const ayni_task_raise_model_limit_v1 body{sizeof(body), command.limit, command.id};
            return submit(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &body, task.revision);
        }
        const ayni_task_target_v1 body{sizeof(body), 0, command.id};
        return submit(AYNI_INTENT_TASK_RESUME_WAIT, &body, task.revision);
    }
    case Operation::cancel: {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        const auto status = ayni_get_goal(node.kernel(), command.id, &goal);
        if (status != AYNI_OK) {
            return failure(status);
        }
        const ayni_goal_target_v1 body{sizeof(body), 0, command.id};
        return submit(AYNI_INTENT_GOAL_CANCEL, &body, goal.revision);
    }
    case Operation::erase: {
        return failure(conversation.forget(
            command.id == 0 ? AYNI_CONTEXT_ERASE_ALL : AYNI_CONTEXT_ERASE_ROUND, command.id));
    }
    default:
        return failure(AYNI_UNSUPPORTED);
    }
}
// These are copied public views, not another state owner. The CLI chooses
// language and formatting; no presentation text or private Core pointers escape.
Reply Assistant::Impl::describe(const Command &command) const {
    Reply reply;
    if (command.operation == Operation::status || command.operation == Operation::diagnostics) {
        reply.kernel.struct_size = sizeof(reply.kernel);
        reply.status = ayni_get_kernel_info(node.kernel(), &reply.kernel);
        reply.privacy.struct_size = sizeof(reply.privacy);
        if (reply.status == AYNI_OK) {
            reply.status = ayni_get_privacy_status(node.kernel(), session, &reply.privacy);
        }
        if (reply.status == AYNI_OK) {
            reply.status = context_info(reply.context);
        }
        const auto *record = conversation.record();
        reply.last_round = record ? record->last_round : reply.context.last_turn_id;
        reply.pending_goal = conversation.active() && !record ? conversation.latest_goal() : 0;
        reply.storage_status = storage_status;
        reply.saving = conversation.file_pending();
        reply.pending_commands = pending_count;
        if (reply.status == AYNI_OK) {
            reply.status = applications::assistant::project(node.kernel(), reply.activity);
        }
    } else if (command.operation == Operation::notifications) {
        std::vector<uint64_t> objects;
        reply.status = ids(AYNI_OBJECT_CALL, objects);
        for (const auto id : objects) {
            if (reply.status != AYNI_OK) {
                break;
            }
            ayni_call_info_v1 call{};
            call.struct_size = sizeof(call);
            reply.status = ayni_get_call(node.kernel(), id, &call);
            if (reply.status != AYNI_OK) {
                break;
            }
            ayni_task_info_v1 task{};
            task.struct_size = sizeof(task);
            reply.status = ayni_get_task(node.kernel(), call.task_id, &task);
            if (reply.status != AYNI_OK) {
                break;
            }
            ayni_goal_info_v1 goal{};
            goal.struct_size = sizeof(goal);
            reply.status = ayni_get_goal(node.kernel(), task.goal_id, &goal);
            if (reply.status == AYNI_OK &&
                std::string(goal.goal_type_id, goal.goal_type_id_size) == "assistant.reminder") {
                reply.presentations.push_back(applications::assistant::presentation(call));
            }
        }
    } else if (command.operation == Operation::conversation) {
        const auto *available = conversation.record();
        if (!available) {
            reply.status = AYNI_NOT_READY;
            return reply;
        }
        const auto &record = *available;
        auto &view = reply.conversation;
        view.generation = record.generation;
        view.revision = record.revision;
        const auto &body_store = *bodies.store();
        std::vector<strategies::Round> rounds;
        reply.status = strategies::Conversation::text(body_store, record.summary, view.summary);
        if (reply.status == AYNI_OK) {
            reply.status = strategies::Conversation::text(body_store, record.draft, view.draft);
        }
        if (reply.status == AYNI_OK) {
            reply.status = strategies::Conversation::rounds(body_store, record, rounds);
        }
        for (auto &round : rounds) {
            view.turns.push_back({round.number, std::move(round.user), std::move(round.answer)});
        }
    } else {
        const auto kind = command.operation == Operation::goals   ? AYNI_OBJECT_GOAL
                          : command.operation == Operation::tasks ? AYNI_OBJECT_TASK
                          : command.operation == Operation::calls ? AYNI_OBJECT_CALL
                                                                  : AYNI_OBJECT_GRANT;
        std::vector<uint64_t> objects;
        reply.status = ids(kind, objects);
        if (reply.status != AYNI_OK) {
            return reply;
        }
        std::sort(objects.begin(), objects.end());
        for (const auto id : objects) {
            if (kind == AYNI_OBJECT_GOAL) {
                ayni_goal_info_v1 goal{};
                goal.struct_size = sizeof(goal);
                reply.status = ayni_get_goal(node.kernel(), id, &goal);
                reply.goals.push_back(goal);
            } else if (kind == AYNI_OBJECT_TASK) {
                ayni_task_info_v1 task{};
                task.struct_size = sizeof(task);
                reply.status = ayni_get_task(node.kernel(), id, &task);
                reply.tasks.push_back(task);
            } else if (kind == AYNI_OBJECT_CALL) {
                ayni_call_info_v1 call{};
                call.struct_size = sizeof(call);
                reply.status = ayni_get_call(node.kernel(), id, &call);
                reply.calls.push_back(call);
            } else {
                ayni_grant_info_v1 grant{};
                grant.struct_size = sizeof(grant);
                reply.status = ayni_get_grant(node.kernel(), id, &grant);
                reply.grants.push_back(grant);
            }
            if (reply.status != AYNI_OK) {
                return reply;
            }
        }
    }
    return reply;
}

Reply Assistant::execute(const Command &command) {
    if (!impl_ || !impl_->opened || impl_->shutting_down) {
        return failure(AYNI_INVALID_STATE);
    }
    try {
        // Stop never waits behind a file save or a failed store, and never reaches a model.
        if (command.operation == Operation::stop ||
            (command.operation == Operation::say &&
             applications::assistant::stop_request(command.text))) {
            return impl_->stop_work();
        }
        if (command.operation == Operation::conversation) {
            // Preserve the synchronous CLI query using bounded local I/O only. No Core
            // step, model dispatch or UI observation is consumed while cold bodies load.
            const auto deadline = std::chrono::steady_clock::now() + local_io_timeout;
            do {
                auto reply = impl_->describe(command);
                const bool readable = impl_->conversation.record() != nullptr;
                if (reply.status != AYNI_NOT_READY || !readable) {
                    if (reply.status == AYNI_OK && !readable) {
                        return failure(AYNI_EXPIRED);
                    }
                    return reply;
                }
                FileBodies::ready(&impl_->bodies, 0);
                const auto stored = FileBodies::status(&impl_->bodies);
                if (stored != AYNI_OK) {
                    return failure(stored);
                }
                impl_->node.wait_for_wake(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            return failure(AYNI_BUSY);
        }
        switch (command.operation) {
        case Operation::status:
        case Operation::diagnostics:
        case Operation::notifications:
        case Operation::goals:
        case Operation::tasks:
        case Operation::calls:
        case Operation::grants:
        case Operation::conversation:
            return impl_->describe(command);
        default:
            break;
        }
        if (impl_->storage_status != AYNI_OK) {
            return failure(impl_->storage_status);
        }
        // File admission stays closed while saving; Core separately pins a published Task
        // until the matching file ACK so unrelated management work cannot retire its source.
        if (impl_->conversation.file_pending()) {
            return failure(AYNI_BUSY);
        }
        return impl_->manage(command);
    } catch (const std::bad_alloc &) {
        return failure(AYNI_CAPACITY_EXCEEDED);
    }
}
} // namespace ayni::runtime
