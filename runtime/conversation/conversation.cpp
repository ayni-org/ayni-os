#include <ayni/memory.h>
#include <ayni/runtime/conversation.h>
#include <cstring>
namespace ayni::runtime::conversation {
using hosts::storage::FileValueRequest;
using strategies::Conversation;
ayni_status_t Session::context(ayni_context_info_v1 &out) const {
    out = initialized<ayni_context_info_v1>();
    return ayni_get_context(config_.kernel, config_.session, config_.owner_instance, &out);
}
bool Session::matches(const strategies::ConversationRecord &r,
                      const ayni_context_info_v1 &c) const {
    return r.id == c.context_id && r.owner_instance_id == c.owner_instance_id &&
           r.generation == c.revision && r.revision == c.generation &&
           r.last_round == c.last_turn_id;
}
ayni_status_t Session::intake(ayni_context_info_v1 &anchor) const {
    if (phase_ == Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    if (phase_ == Phase::failed) {
        return failure_;
    }
    if (phase_ != Phase::ready) {
        return AYNI_BUSY;
    }
    const auto status = context(anchor);
    if (status != AYNI_OK) {
        return status;
    }
    return anchor.usable && anchor.pending_action == AYNI_CONTEXT_IDLE && matches(current_, anchor)
               ? AYNI_OK
               : AYNI_BUSY;
}
bool Session::ready() const {
    auto anchor = initialized<ayni_context_info_v1>();
    return intake(anchor) == AYNI_OK;
}
ayni_status_t Session::configure(Config config) {
    if (phase_ != Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    if (!config.kernel || !config.session || !config.owner_instance || !config.goal_type.data ||
        !config.goal_type.size || !config.workspace || !config.files.submit || !config.bodies ||
        !config.commands.submit) {
        return AYNI_INVALID_ARGUMENT;
    }
    config_ = config;
    auto anchor = initialized<ayni_context_info_v1>();
    const auto status = context(anchor);
    if (status != AYNI_OK) {
        return status;
    }
    if (!anchor.usable || anchor.pending_action != AYNI_CONTEXT_IDLE) {
        return AYNI_NOT_READY;
    }
    context_id_ = anchor.context_id;
    phase_ = Phase::load;
    return AYNI_OK;
}
void Session::complete(void *context, const hosts::storage::FileValueResult &result) noexcept {
    auto &self = *static_cast<Session *>(context);
    self.answer_ = result;
    self.completed_.store(true, std::memory_order_release);
}
Submission Session::command(ayni_intent_kind_t kind, const ayni_context_info_v1 &anchor,
                            uint64_t task, ayni_context_action_t action, uint64_t turn_id) {
    const ayni_context_command_v1 body{sizeof(body),      action, 0,      context_id_,
                                       anchor.generation, task,   turn_id};
    return config_.commands.submit(config_.commands.context, kind, &body, anchor.revision);
}
// Missing body initializes only an empty anchor. Existing bodies must match the
// complete anchor/owner and contain no unfinished draft; no empty fallback is allowed.
ayni_status_t Session::load(const ayni_context_info_v1 &anchor) {
    if (phase_ == Phase::load) {
        FileValueRequest request;
        request.key = {config_.owner_instance, context_id_};
        request.buffer = file_.bytes.data();
        request.size = static_cast<uint32_t>(file_.bytes.size());
        request.context = this;
        request.complete = complete;
        if (config_.files.submit(config_.files.context, &request) != AYNI_SUBMIT_ACCEPTED) {
            return AYNI_BUSY;
        }
        phase_ = Phase::loading;
        return AYNI_OK;
    }
    if (!completed_.exchange(false, std::memory_order_acquire)) {
        return AYNI_OK;
    }
    if (answer_.status == AYNI_NOT_FOUND) {
        if (anchor.generation || anchor.last_turn_id ||
            anchor.pending_action != AYNI_CONTEXT_IDLE) {
            return AYNI_NOT_READY;
        }
        candidate_.id = context_id_;
        candidate_.owner_instance_id = config_.owner_instance;
        candidate_.generation = anchor.revision;
        candidate_.revision = anchor.generation;
        phase_ = Phase::save;
        save_ = Save::initialize;
        return AYNI_OK;
    }
    if (answer_.status != AYNI_OK) {
        return answer_.status;
    }
    const auto status =
        Conversation::decode(config_.workspace, {file_.bytes.data(), answer_.size}, current_);
    if (status != AYNI_OK) {
        return status;
    }
    if (!matches(current_, anchor) || current_.draft.size) {
        return AYNI_NOT_READY;
    }
    value_version_ = answer_.generation;
    phase_ = Phase::ready;
    return AYNI_OK;
}
ayni_status_t Session::save() {
    if (value_version_ == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = Conversation::encode(config_.workspace, candidate_, file_);
    if (status != AYNI_OK) {
        return status;
    }
    FileValueRequest request;
    request.kind = FileValueRequest::Kind::write;
    request.key = {config_.owner_instance, context_id_};
    request.previous_generation = value_version_;
    request.generation = value_version_ + 1;
    request.data = file_.bytes.data();
    request.size = file_.size;
    request.context = this;
    request.complete = complete;
    if (config_.files.submit(config_.files.context, &request) != AYNI_SUBMIT_ACCEPTED) {
        return AYNI_BUSY;
    }
    phase_ = Phase::saving;
    return AYNI_OK;
}
ayni_status_t Session::saved(const ayni_context_info_v1 &anchor) {
    if (!completed_.exchange(false, std::memory_order_acquire)) {
        return AYNI_OK;
    }
    if (answer_.status != AYNI_OK) {
        return answer_.status; // UNKNOWN is preserved and never acknowledged to Core.
    }
    value_version_ = answer_.generation;
    if (!matches(candidate_, anchor)) {
        // Privacy may have changed while I/O was in flight. Withhold all text and
        // stop this application; the still-pending Core action is never falsely ACKed.
        return AYNI_NOT_READY;
    }
    current_ = candidate_;
    if (save_ == Save::publication || save_ == Save::erasure) {
        // No turn is outstanding: bodies this record does not name are superseded or erased.
        std::vector<strategies::BodyRef> keep;
        Conversation::references(current_, keep);
        config_.bodies->collect(current_.id, keep);
    }
    phase_ = save_ == Save::draft        ? Phase::start_turn
             : save_ == Save::initialize ? Phase::ready
                                         : Phase::acknowledge;
    return AYNI_OK;
}
ayni_status_t Session::begin(ayni_string_v1 user, uint64_t memory_id) {
    auto anchor = initialized<ayni_context_info_v1>();
    const auto available = intake(anchor);
    if (available != AYNI_OK) {
        return available;
    }
    input_.assign(user.data ? user.data : "", user.size);
    memory_id_ = memory_id;
    const auto status = input();
    if (status != AYNI_OK && status != AYNI_NOT_READY) {
        input_.clear();
        return status;
    }
    phase_ = status == AYNI_OK ? Phase::save : Phase::input;
    return AYNI_OK;
}
// Stores the accepted input as a body; NOT_READY until the body store made it durable.
ayni_status_t Session::input() {
    const auto status = Conversation::begin(config_.workspace, *config_.bodies->store(), current_,
                                            {input_.data(), static_cast<uint32_t>(input_.size())},
                                            parameters_, memory_id_);
    if (status != AYNI_OK) {
        return status;
    }
    candidate_ = current_;
    const auto copied =
        Conversation::input(config_.workspace, parameters_.view(), candidate_.draft);
    if (copied != AYNI_OK) {
        return copied;
    }
    input_.clear();
    save_ = Save::draft;
    return AYNI_OK;
}
// Core owns Goal/Task lifecycle and the ToolLoop. This application only submits
// the copied user turn and bridges a completed Task to the outer context file.
ayni_status_t Session::turn(const ayni_context_info_v1 &anchor) {
    if (phase_ == Phase::start_turn) {
        // Privacy maintenance retains terminal work until compaction finishes.
        // Wait before new Goal admission; file ACKs in other Sessions must still progress.
        auto privacy = initialized<ayni_privacy_info_v1>();
        const auto queried = ayni_get_privacy_status(config_.kernel, config_.session, &privacy);
        if (queried != AYNI_OK) {
            return queried;
        }
        if (privacy.phase != AYNI_PRIVACY_IDLE && privacy.phase != AYNI_PRIVACY_COMPLETE) {
            return AYNI_BUSY;
        }
        // begin() already encoded the exact input before the durable draft save.
        // Re-encoding against that saved draft would append the same input twice.
        auto body = initialized<ayni_goal_create_v1>();
        body.goal_type_id = config_.goal_type;
        body.goal_type_version = 1;
        body.owner_instance_id = config_.owner_instance;
        body.parameters = parameters_.view();
        body.trigger = AYNI_TRIGGER_ONCE;
        const auto accepted =
            config_.commands.submit(config_.commands.context, AYNI_INTENT_GOAL_CREATE, &body, 0);
        if (accepted.status == AYNI_OK) {
            goal_ = accepted.result.object.id;
            phase_ = Phase::turn;
        }
        return accepted.status;
    }
    auto goal = initialized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(config_.kernel, goal_, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    if (!goal.last_task_id) {
        return AYNI_OK;
    }
    auto task = initialized<ayni_task_info_v1>();
    status = ayni_get_task(config_.kernel, goal.last_task_id, &task);
    if (status != AYNI_OK) {
        return status;
    }
    if (task.lifecycle == AYNI_TASK_PAUSED && task.hold_status != AYNI_OK) {
        return task.hold_status; // Report the Core hold; poll keeps the original Task attached.
    }
    if (task.lifecycle == AYNI_TASK_COMPLETED) {
        const auto accepted = command(AYNI_INTENT_CONTEXT_PUBLISH, anchor, task.task_id,
                                      AYNI_CONTEXT_IDLE, anchor.last_turn_id + 1);
        if (accepted.status == AYNI_OK) {
            phase_ = Phase::publish;
        }
        return accepted.status;
    }
    if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_EXPIRED ||
        task.lifecycle == AYNI_TASK_CANCELLED) {
        return AYNI_NOT_READY; // Preserve draft and evidence; never manufacture an answer.
    }
    return AYNI_OK;
}
ayni_status_t Session::publish(const ayni_context_info_v1 &anchor) {
    if (anchor.pending_action != AYNI_CONTEXT_PUBLISH || !anchor.pending_task_id) {
        return AYNI_NOT_READY;
    }
    const auto copied = ayni_copy_task_checkpoint(
        config_.kernel, anchor.pending_task_id, parameters_.bytes.data(),
        static_cast<uint32_t>(parameters_.bytes.size()), &parameters_.size);
    if (copied != AYNI_OK) {
        return copied;
    }
    const auto status =
        Conversation::finish(config_.workspace, current_, parameters_.view(), candidate_);
    if (status != AYNI_OK) {
        return status;
    }
    if (!matches(candidate_, anchor)) {
        return AYNI_REVISION_CONFLICT;
    }
    save_ = Save::publication;
    phase_ = Phase::save;
    return AYNI_OK;
}
ayni_status_t Session::forget(ayni_context_action_t action, uint64_t turn_id) {
    auto anchor = initialized<ayni_context_info_v1>();
    const auto queried = intake(anchor);
    if (queried != AYNI_OK) {
        return queried;
    }
    const auto accepted = command(AYNI_INTENT_CONTEXT_FORGET, anchor, 0, action, turn_id);
    if (accepted.status == AYNI_OK) {
        phase_ = Phase::erase;
    }
    return accepted.status;
}
ayni_status_t Session::erase(const ayni_context_info_v1 &anchor) {
    if (anchor.pending_action < AYNI_CONTEXT_ERASE_ALL) {
        return AYNI_NOT_READY;
    }
    const auto &bodies = *config_.bodies->store();
    const auto status =
        anchor.pending_action == AYNI_CONTEXT_ERASE_ALL
            ? Conversation::erase(config_.workspace, current_, candidate_)
        : anchor.pending_action == AYNI_CONTEXT_ERASE_ROUND
            ? Conversation::erase_round(config_.workspace, bodies, current_, anchor.pending_turn_id,
                                        candidate_)
            : Conversation::erase_derived(config_.workspace, bodies, current_, candidate_);
    if (status == AYNI_NOT_READY) {
        return AYNI_OK; // The kept inputs' body is still being stored; retried next poll.
    }
    if (status != AYNI_OK) {
        return status;
    }
    candidate_.generation = anchor.revision;
    candidate_.revision = anchor.generation;
    save_ = Save::erasure;
    phase_ = Phase::save;
    return AYNI_OK;
}
// Each invocation performs one queue operation or one public state transition.
// No owner text escapes until both the file and the exact Core ACK are durable.
ayni_status_t Session::poll() {
    if (phase_ == Phase::failed) {
        return failure_;
    }
    if (phase_ == Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    auto anchor = initialized<ayni_context_info_v1>();
    auto status = context(anchor);
    auto kernel = initialized<ayni_kernel_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_kernel_info(config_.kernel, &kernel);
    }
    if (status != AYNI_OK || anchor.commit_seq > kernel.durable_seq) {
        return status;
    }
    switch (phase_) {
    case Phase::load:
    case Phase::loading:
        status = load(anchor);
        break;
    case Phase::input:
        status = input();
        if (status == AYNI_OK) {
            phase_ = Phase::save;
        } else if (status == AYNI_NOT_READY) {
            status = AYNI_OK;
        }
        break;
    case Phase::save:
        status = save();
        break;
    case Phase::saving:
        status = saved(anchor);
        break;
    case Phase::start_turn:
    case Phase::turn:
        status = turn(anchor);
        break;
    case Phase::publish:
        status = publish(anchor);
        break;
    case Phase::erase:
        status = erase(anchor);
        break;
    case Phase::acknowledge:
        if (!matches(current_, anchor)) {
            status = AYNI_NOT_READY;
        } else {
            status = command(AYNI_INTENT_CONTEXT_ACK, anchor).status;
            if (status == AYNI_OK) {
                phase_ = Phase::acknowledged;
            }
        }
        break;
    case Phase::acknowledged:
        if (anchor.pending_action == AYNI_CONTEXT_IDLE && anchor.usable &&
            matches(current_, anchor)) {
            phase_ = Phase::ready;
        }
        break;
    case Phase::ready:
        if (anchor.pending_action >= AYNI_CONTEXT_ERASE_ALL) {
            phase_ = Phase::erase;
        } else if (!anchor.usable) {
            status =
                command(AYNI_INTENT_CONTEXT_FORGET, anchor, 0, AYNI_CONTEXT_ERASE_DERIVED).status;
            if (status == AYNI_OK) {
                phase_ = Phase::erase;
            }
        }
        break;
    default:
        status = AYNI_INVALID_STATE;
        break;
    }
    // Core owns Task holds, including owner-recoverable authority/model overlays.
    // Report their status without detaching from the original Task; polling after
    // owner resolution continues that Task and does not resubmit its old actions.
    if (status != AYNI_OK && status != AYNI_BUSY && phase_ != Phase::turn) {
        failure_ = status;
        phase_ = Phase::failed;
    }
    return status;
}
} // namespace ayni::runtime::conversation
