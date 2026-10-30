// Kernel owns source/version barriers; this adapter owns the matching outer file and user notices.
#include "assistant_state.h"
#include <chrono>
namespace ayni::runtime {
namespace {
constexpr uint64_t conversation_id = 1;
constexpr uint32_t step_units = 64;
constexpr uint64_t step_us = 10000;
Notice published_notice(const SavedConversation &file) {
    if (file.record.round_count == 0) {
        return {};
    }
    const auto &answer = file.record.rounds[file.record.round_count - 1].answer;
    return {NoticeKind::answer, std::string(answer.bytes.data(), answer.size)};
}
} // namespace
ayni_status_t Assistant::Impl::context_info(ayni_context_info_v1 &context) const {
    context = {};
    context.struct_size = sizeof(context);
    return ayni_get_context(node.kernel(), session, assistant(), &context);
}
Reply Assistant::Impl::context_command(ayni_intent_kind_t kind, const ayni_context_info_v1 &context,
                                       uint64_t task, ayni_context_action_t action, uint64_t turn) {
    const ayni_context_command_v1 body{sizeof(body),       action, 0,   context.context_id,
                                       context.generation, task,   turn};
    return submit(kind, &body, context.revision);
}
// Missing files may initialize only an empty anchor. A retained conversation cannot silently
// become empty, and an old untracked Goal cannot be adopted by the new context mechanism.
ayni_status_t Assistant::Impl::initialize_context(bool missing) {
    ayni_context_info_v1 context{};
    auto status = context_info(context);
    if (status == AYNI_NOT_FOUND) {
        if (!missing) {
            return AYNI_NOT_READY;
        }
        std::vector<uint64_t> goals;
        status = ids(AYNI_OBJECT_GOAL, goals);
        if (status != AYNI_OK) {
            return status;
        }
        for (auto id : goals) {
            ayni_goal_info_v1 goal{};
            goal.struct_size = sizeof(goal);
            status = ayni_get_goal(node.kernel(), id, &goal);
            if (status != AYNI_OK || goal.owner_instance_id == assistant()) {
                return status == AYNI_OK ? AYNI_NOT_READY : status;
            }
        }
        ayni_context_command_v1 body{};
        body.struct_size = sizeof(body);
        body.owner_instance_id = assistant();
        const auto accepted = submit(AYNI_INTENT_CONTEXT_OPEN, &body);
        if (accepted.status != AYNI_OK) {
            return accepted.status;
        }
        status = context_info(context);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (missing) {
        if (context.generation != 0 || context.last_turn_id != 0 ||
            context.pending_action != AYNI_CONTEXT_IDLE) {
            return AYNI_NOT_READY;
        }
        current.context_id = context.context_id;
        current.record.id = conversation_id;
        current.record.owner_instance_id = context.owner_instance_id;
    }
    if (current.record.id != conversation_id || current.context_id != context.context_id ||
        current.record.owner_instance_id != context.owner_instance_id ||
        current.record.generation > context.revision ||
        current.record.revision > context.generation ||
        (context.pending_action == AYNI_CONTEXT_IDLE && !matches(current, context))) {
        return AYNI_DENIED;
    }
    if (!missing) {
        return AYNI_OK;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        ayni_kernel_info_v1 info{};
        info.struct_size = sizeof(info);
        status = ayni_get_kernel_info(node.kernel(), &info);
        if (status != AYNI_OK || info.durable_seq >= context.commit_seq) {
            break;
        }
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        status = ayni_step(node.kernel(), step_units, step_us, &report);
        if (status != AYNI_OK) {
            return status;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    } while (true);
    return status == AYNI_OK ? persist(current) : status;
}
// A worker may finish after a deletion was accepted. Its bytes can be overwritten, but never
// adopted or acknowledged under the newer revision. Notice delivery crosses the ACK barrier too.
ayni_status_t Assistant::Impl::collect_file(const ayni_context_info_v1 &context,
                                            std::vector<Notice> &notices) {
    if (saving) {
        ayni_status_t result = AYNI_OK;
        if (!store.poll(result)) {
            return AYNI_OK;
        }
        if (result != AYNI_OK) {
            storage_status = result;
            saving.reset();
            return result;
        }
        if (matches(saving->candidate, context) &&
            (context.usable || context.pending_action >= AYNI_CONTEXT_ERASE_ALL)) {
            current = saving->candidate;
            if (saving->notice.kind != NoticeKind::none) {
                awaiting_notice =
                    AwaitingNotice{context.revision, context.generation, saving->notice};
            }
        }
        saving.reset();
    }
    if (awaiting_notice) {
        if (awaiting_notice->revision != context.revision ||
            awaiting_notice->generation != context.generation || !context.usable) {
            // Erasure notices are also withheld until the file action has been acknowledged.
            if (context.pending_action < AYNI_CONTEXT_ERASE_ALL ||
                awaiting_notice->revision != context.revision ||
                awaiting_notice->generation != context.generation) {
                awaiting_notice.reset();
            }
        } else if (context.pending_action == AYNI_CONTEXT_IDLE) {
            notices.push_back(std::move(awaiting_notice->notice));
            awaiting_notice.reset();
        }
    }
    return AYNI_OK;
}
ayni_status_t Assistant::Impl::publish_context(const ayni_context_info_v1 &context) {
    strategies::Encoded encoded;
    const auto copied =
        ayni_copy_task_checkpoint(node.kernel(), context.pending_task_id, encoded.bytes.data(),
                                  strategies::state_bytes_max, &encoded.size);
    if (copied != AYNI_OK) {
        return copied;
    }
    auto candidate = current;
    const auto status =
        strategies::Conversation::finish(current.record, encoded.view(), candidate.record);
    if (status != AYNI_OK) {
        return status;
    }
    candidate.pending_goal = 0;
    if (!matches(candidate, context)) {
        return AYNI_REVISION_CONFLICT;
    }
    return persist(candidate, published_notice(candidate));
}
// Invalidation is already durable. Clearing generated answers is conservative; undiscarded user
// inputs stay independent facts. The original Call ledger keeps actual physical outcomes.
ayni_status_t Assistant::Impl::erase_context(const ayni_context_info_v1 &context) {
    const auto cancelled = cancel_pending_turn().status;
    if (cancelled != AYNI_OK) {
        return cancelled == AYNI_BUSY ? AYNI_OK : cancelled; // Retried on the next pass.
    }
    auto candidate = current;
    auto base = current.record;
    // Keep the original published turn ID; reclassifying it as a later input would defeat
    // a subsequent single-turn delete. begin reserves this one bounded retention slot.
    if (context.last_turn_id > base.last_round) {
        const auto retained = strategies::Conversation::retain_published_input(
            current.record, context.last_turn_id, base);
        if (retained != AYNI_OK) {
            return retained;
        }
    }
    const auto status =
        context.pending_action == AYNI_CONTEXT_ERASE_ALL
            ? strategies::Conversation::erase(base, candidate.record)
        : context.pending_action == AYNI_CONTEXT_ERASE_ROUND
            ? strategies::Conversation::erase_round(base, context.pending_turn_id, candidate.record)
            : strategies::Conversation::erase_derived(base, candidate.record);
    if (status != AYNI_OK) {
        return status;
    }
    candidate.record.generation = context.revision;
    candidate.record.revision = context.generation;
    candidate.pending_goal = 0;
    const auto erasure =
        context.pending_action == AYNI_CONTEXT_ERASE_ALL     ? ConversationStore::Erasure::all
        : context.pending_action == AYNI_CONTEXT_ERASE_ROUND ? ConversationStore::Erasure::turn
                                                             : ConversationStore::Erasure::derived;
    return persist(candidate, {NoticeKind::context_erased, {}}, erasure, context.pending_turn_id);
}
// One step advances at most one outer save or Kernel intent. All I/O remains on the store worker.
ayni_status_t Assistant::Impl::collect(std::vector<Notice> &notices) {
    if (storage_status != AYNI_OK) {
        return storage_status;
    }
    ayni_context_info_v1 context{};
    auto status = context_info(context);
    if (status != AYNI_OK) {
        return status;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    status = ayni_get_kernel_info(node.kernel(), &info);
    if (status != AYNI_OK || context.commit_seq > info.durable_seq) {
        return status;
    }
    status = collect_file(context, notices);
    if (status != AYNI_OK || saving) {
        return status;
    }
    if (!context.usable && context.pending_action < AYNI_CONTEXT_ERASE_ALL) {
        // Captured permissions may expire without a memory tombstone. The trusted runtime
        // removes generated context through the same explicit owner intent before reuse.
        status = context_command(AYNI_INTENT_CONTEXT_FORGET, context, 0, AYNI_CONTEXT_ERASE_DERIVED)
                     .status;
        return status == AYNI_BUSY ? AYNI_OK : status;
    }
    if (context.pending_action != AYNI_CONTEXT_IDLE) {
        if (!matches(current, context)) {
            return context.pending_action == AYNI_CONTEXT_PUBLISH ? publish_context(context)
                                                                  : erase_context(context);
        }
        if (!awaiting_notice) {
            awaiting_notice = AwaitingNotice{context.revision, context.generation,
                                             context.pending_action == AYNI_CONTEXT_PUBLISH
                                                 ? published_notice(current)
                                                 : Notice{NoticeKind::context_erased, {}}};
        }
        status = context_command(AYNI_INTENT_CONTEXT_ACK, context).status;
        // Maintenance and bounded command slots provide normal backpressure; keep the file
        // and notice pending and retry the same version on the next owner-thread step.
        // A captured Grant can expire between the query and ACK; the next collect rechecks
        // usability and requests derived erasure before exposing the withheld notice.
        return status == AYNI_BUSY || status == AYNI_EXPIRED ? AYNI_OK : status;
    }
    return collect_turn();
}
// Completed Tasks first publish their exact sources into the Kernel anchor. The next collect
// reconstructs the file from that retained Task only after publication is durable.
ayni_status_t Assistant::Impl::collect_turn() {
    if (!current.pending_goal) {
        return AYNI_OK;
    }
    ayni_goal_info_v1 goal{};
    goal.struct_size = sizeof(goal);
    auto status = ayni_get_goal(node.kernel(), current.pending_goal, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    if (goal.owner_instance_id != current.record.owner_instance_id) {
        return AYNI_DENIED;
    }
    bool aborted = goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_EXPIRED ||
                   goal.lifecycle == AYNI_GOAL_FAILED;
    if (goal.last_task_id != 0) {
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        status = ayni_get_task(node.kernel(), goal.last_task_id, &task);
        if (status != AYNI_OK) {
            return status;
        }
        if (task.lifecycle == AYNI_TASK_COMPLETED) {
            ayni_context_info_v1 context{};
            status = context_info(context);
            if (status != AYNI_OK) {
                return status;
            }
            status = context_command(AYNI_INTENT_CONTEXT_PUBLISH, context, task.task_id,
                                     AYNI_CONTEXT_IDLE, context.last_turn_id + 1)
                         .status;
            return status == AYNI_BUSY ? AYNI_OK : status;
        }
        aborted = aborted || task.lifecycle == AYNI_TASK_FAILED ||
                  task.lifecycle == AYNI_TASK_CANCELLED || task.lifecycle == AYNI_TASK_EXPIRED;
    }
    if (aborted) {
        auto candidate = current;
        candidate.pending_goal = 0;
        return persist(candidate, {NoticeKind::turn_aborted, {}});
    }
    return AYNI_OK;
}
} // namespace ayni::runtime
