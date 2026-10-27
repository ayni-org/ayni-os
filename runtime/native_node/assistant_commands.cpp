#include "assistant_state.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace ayni::runtime {
namespace {
Reply failure(ayni_status_t status) {
    Reply reply;
    reply.status = status;
    return reply;
}
// CLI command identities occupy a boot-specific monotonic range. These are
// management keys, not Kernel object IDs; no persistent counter shadows Core.
constexpr uint64_t commands_per_boot = uint64_t{1} << 32;

} // namespace
Reply Assistant::Impl::submit(ayni_intent_kind_t kind, const void *body, uint32_t revision) {
    const auto collected = collect_receipts();
    if (collected != AYNI_OK) {
        return failure(collected);
    }
    if (pending_count == pending_commands.size()) {
        return failure(AYNI_BUSY);
    }
    if (!boot || boot >= std::numeric_limits<uint64_t>::max() / commands_per_boot ||
        command_sequence == commands_per_boot - 1) {
        return failure(AYNI_CAPACITY_EXCEEDED);
    }
    ayni_intent_v1 intent{sizeof(intent),
                          kind,
                          boot * commands_per_boot + ++command_sequence,
                          session,
                          revision,
                          0,
                          body};
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
Reply Assistant::Impl::say(const Command &request) {
    Command command = request;
    if (!command.agent) {
        command.agent =
            applications::assistant::route(request.text, demo_agent_a, demo_agent_b, command.text);
    }
    if (command.agent != demo_agent_a && command.agent != demo_agent_b) {
        return failure(AYNI_INVALID_ARGUMENT);
    }
    const bool chat = command.agent == demo_agent_b;
    if ((chat && current.pending_goal) || (!chat && rule_goal)) {
        return failure(AYNI_BUSY);
    }
    if (!chat && command.text != "check" && command.text != "检查") {
        auto reply = failure(AYNI_UNSUPPORTED);
        reply.message = Message::rule_usage;
        return reply;
    }
    strategies::Encoded parameters;
    strategies::Text<strategies::turn_text_max> input;
    if (chat) {
        const auto status = strategies::Conversation::begin(
            current.record, runtime_text(command.text), parameters, command.id);
        if (status != AYNI_OK) {
            return failure(status);
        }
        const auto decoded = strategies::Conversation::input(parameters.view(), input);
        if (decoded != AYNI_OK) {
            return failure(decoded);
        }
    }
    ayni_goal_create_v1 goal{};
    goal.struct_size = sizeof(goal);
    goal.goal_type_version = 1;
    goal.goal_type_id = chat ? runtime_text("chat") : runtime_text("rule.check");
    goal.owner_instance_id = instance(command.agent);
    goal.parameters = parameters.view();
    goal.trigger = AYNI_TRIGGER_ONCE;
    auto reply = submit(AYNI_INTENT_GOAL_CREATE, &goal);
    if (reply.status != AYNI_OK) {
        return reply;
    }
    if (chat) {
        // Remember accepted work even if the outer save fails. Never submit a
        // second turn against the same baseline while this Goal exists.
        current.pending_goal = reply.id;
        current.record.draft = input;
        reply.status = persist(current);
    } else {
        rule_goal = reply.id;
    }
    return reply;
}
// The approval echo is read through public APIs, with the same immutable
// creation parameters used for reauthorization. Nothing reconstructs Core records.
Reply Assistant::Impl::approve(uint64_t id, Operation operation) {
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    auto status = ayni_get_goal(node.kernel(), id, &info);
    if (status != AYNI_OK) {
        return failure(status);
    }
    strategies::Encoded parameters;
    status = ayni_get_goal_parameters(node.kernel(), id, parameters.bytes.data(),
                                      strategies::state_bytes_max, &parameters.size);
    if (status != AYNI_OK) {
        return failure(status);
    }
    ayni_goal_create_v1 original{};
    original.struct_size = sizeof(original);
    original.owner_instance_id = info.owner_instance_id;
    original.parent_goal_id = info.parent_goal_id;
    original.pause_group = info.pause_group;
    original.goal_type_id = {info.goal_type_id, info.goal_type_id_size};
    original.goal_type_version = info.goal_type_version;
    original.parameters = parameters.view();
    original.trigger = info.trigger;
    ayni_calendar_info_v1 calendar{};
    if (info.trigger == AYNI_TRIGGER_CALENDAR_ONCE || info.trigger == AYNI_TRIGGER_CALENDAR_DAILY ||
        info.trigger == AYNI_TRIGGER_CALENDAR_WEEKLY) {
        calendar.struct_size = sizeof(calendar);
        status = ayni_get_calendar(node.kernel(), id, &calendar);
        if (status != AYNI_OK) {
            return failure(status);
        }
        original.calendar = &calendar.rule;
    }
    original.trigger_delay_us = info.trigger_delay_us;
    original.trigger_period_us = info.trigger_period_us;
    original.expiry_us = info.original_expiry_us;
    original.trigger_source = {info.trigger_source, info.trigger_source_size};
    ayni_goal_authority_info_v1 authority{};
    authority.struct_size = sizeof(authority);
    status = ayni_get_goal_authority(node.kernel(), id, &authority);
    if (status != AYNI_OK) {
        return failure(status);
    }
    if (operation == Operation::reauthorize) {
        const ayni_goal_reauthorize_v1 body{sizeof(body), 0, id, authority.commit_seq, original};
        return submit(AYNI_INTENT_GOAL_REAUTHORIZE, &body, info.revision);
    }
    ayni_goal_approve_v1 body{};
    body.struct_size = sizeof(body);
    body.goal_id = id;
    body.lifetime = AYNI_GRANT_PERMANENT;
    body.original = original;
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
    return submit(AYNI_INTENT_GOAL_APPROVE, &body, info.revision);
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
    case Operation::remind:
    case Operation::device_check:
    case Operation::memory_organize:
    case Operation::scene_observe:
        return application_goal(command);
    case Operation::pause_assistant:
    case Operation::resume_assistant: {
        ayni_pause_group_info_v1 info{};
        info.struct_size = sizeof(info);
        const auto status =
            ayni_get_pause_group(node.kernel(), applications::assistant::autonomous_group, &info);
        if (status != AYNI_OK) {
            return failure(status);
        }
        const ayni_pause_group_set_v1 body{
            sizeof(body), applications::assistant::autonomous_group,
            command.operation == Operation::pause_assistant ? 1U : 0U, 0};
        return submit(AYNI_INTENT_PAUSE_GROUP_SET, &body, info.revision);
    }
    case Operation::grant: {
        const auto cap = profile.capability(command.text);
        if (!cap.size || (command.agent != demo_agent_a && command.agent != demo_agent_b)) {
            return failure(AYNI_INVALID_ARGUMENT);
        }
        const ayni_grant_create_v1 body{sizeof(body),         1, cap, instance(command.agent),    0,
                                        AYNI_GRANT_PERMANENT, 0, 0,   profile.scope(command.text)};
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
        ayni_context_info_v1 context{};
        const auto status = context_info(context);
        if (status != AYNI_OK) {
            return failure(status);
        }
        return context_command(AYNI_INTENT_CONTEXT_FORGET, context, 0,
                               command.id == 0 ? AYNI_CONTEXT_ERASE_ALL : AYNI_CONTEXT_ERASE_ROUND,
                               command.id);
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
        reply.last_round = current.record.last_round;
        reply.pending_goal = current.pending_goal;
        reply.storage_status = storage_status;
        reply.saving = saving.has_value();
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
        ayni_context_info_v1 context{};
        reply.status = context_info(context);
        if (reply.status != AYNI_OK) {
            return reply;
        }
        if (!context.usable || context.pending_action != AYNI_CONTEXT_IDLE ||
            !matches(current, context)) {
            reply.status = context.usable ? AYNI_BUSY : AYNI_EXPIRED;
            return reply;
        }
        const auto &record = current.record;
        auto &view = reply.conversation;
        view.generation = record.generation;
        view.revision = record.revision;
        view.summary.assign(record.summary.bytes.data(), record.summary.size);
        view.draft.assign(record.draft.bytes.data(), record.draft.size);
        for (uint32_t at = 0; at < record.round_count; ++at) {
            const auto &round = record.rounds[at];
            view.turns.push_back({round.number,
                                  std::string(round.user.bytes.data(), round.user.size),
                                  std::string(round.answer.bytes.data(), round.answer.size)});
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
        if (impl_->saving) {
            return failure(AYNI_BUSY);
        }
        return impl_->manage(command);
    } catch (const std::bad_alloc &) {
        return failure(AYNI_CAPACITY_EXCEEDED);
    }
}
} // namespace ayni::runtime
