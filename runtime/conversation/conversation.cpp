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
        !config.goal_type.size || !config.workspace || !config.files.submit ||
        !config.bodies.store || !config.bodies.collect || !config.commands.submit ||
        (config.persistence == strategies::Persistence::background &&
         (!config.bodies.store->stage || !config.bodies.store->share ||
          !config.bodies.store->confirm))) {
        return AYNI_INVALID_ARGUMENT;
    }
    config_ = config;
    auto anchor = initialized<ayni_context_info_v1>();
    const auto status = context(anchor);
    if (status != AYNI_OK) {
        return status;
    }
    const bool finishing_saved_publication =
        config.persistence == strategies::Persistence::background &&
        anchor.pending_action == AYNI_CONTEXT_PUBLISH;
    if (!anchor.usable ||
        (anchor.pending_action != AYNI_CONTEXT_IDLE && !finishing_saved_publication)) {
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
// Missing body initializes only an empty anchor. Existing bodies must match its
// exact owner/version; an explicit discard may remove a settled draft, never adopt it.
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
    value_version_ = answer_.generation;
    if (config_.persistence == strategies::Persistence::background &&
        anchor.pending_action == AYNI_CONTEXT_PUBLISH) {
        if (matches(current_, anchor)) {
            phase_ = Phase::acknowledge; // Exact complete file already saved; no action is resumed.
            return AYNI_OK;
        }
        if (current_.id == anchor.context_id &&
            current_.owner_instance_id == anchor.owner_instance_id &&
            current_.generation == anchor.revision && current_.revision < UINT64_MAX &&
            current_.last_round < UINT64_MAX && current_.revision + 1U == anchor.generation &&
            current_.last_round + 1U == anchor.last_turn_id) {
            phase_ =
                Phase::publish; // Reuse the completed durable checkpoint, never re-run its Task.
            return AYNI_OK;
        }
    }
    if (!matches(current_, anchor)) {
        return AYNI_NOT_READY;
    }
    if (current_.draft.size) {
        if (config_.unfinished != UnfinishedTurn::discard) {
            return AYNI_NOT_READY;
        }
        auto info = initialized<ayni_kernel_info_v1>();
        const auto queried = ayni_get_kernel_info(config_.kernel, &info);
        if (queried != AYNI_OK) {
            return queried;
        }
        const auto stopped = settled(info.durable_seq);
        return stopped == AYNI_OK ? discard() : stopped;
    }
    phase_ = Phase::ready;
    return AYNI_OK;
}
ayni_status_t Session::confirm(const strategies::ConversationRecord &record) {
    const Confirmed key{record.generation,
                        record.revision,
                        {record.summary.id, record.history.id, record.draft.id, record.rounds.id}};
    if (confirmed_ && confirmed_->generation == key.generation &&
        confirmed_->revision == key.revision && confirmed_->refs == key.refs) {
        return AYNI_OK;
    }
    const auto *store = config_.bodies.store;
    if (!store->confirm) {
        return AYNI_OK;
    } // Legacy durable-only descriptor.
    std::vector<strategies::BodyRef> refs;
    auto status = Conversation::references(*store, record, refs);
    if (status != AYNI_OK) {
        return status;
    }
    bool pending = false;
    for (const auto &ref : refs) {
        status = store->confirm(store->context, &ref);
        if (status == AYNI_NOT_READY) {
            // C-HVAL-009: request every dependency in this owner pass. The store
            // still admits one background quantum; publishing waits for all of them.
            pending = true;
            continue;
        }
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (pending) {
        return AYNI_NOT_READY;
    }
    confirmed_ = key;
    return AYNI_OK;
}
ayni_status_t Session::save() {
    const auto durable = confirm(candidate_);
    if (durable != AYNI_OK) {
        return durable == AYNI_NOT_READY ? AYNI_BUSY : durable;
    }
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
    const auto previous = current_;
    current_ = candidate_;
    if (save_ == Save::publication || save_ == Save::erasure || save_ == Save::discard) {
        superseded_ = previous;
    }
    phase_ = save_ == Save::draft                                  ? Phase::start_turn
             : save_ == Save::initialize || save_ == Save::discard ? Phase::ready
                                                                   : Phase::acknowledge;
    if (superseded_) {
        phase_ = Phase::collect;
    }
    if (save_ == Save::discard) {
        end_request_ = EndRequest::none;
    }
    return AYNI_OK;
}
ayni_status_t Session::begin(ayni_string_v1 user, uint64_t memory_id) {
    auto anchor = initialized<ayni_context_info_v1>();
    const auto available = intake(anchor);
    if (available != AYNI_OK) {
        return available;
    }
    if (!user.data || !user.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    input_.assign(user.data, user.size);
    memory_id_ = memory_id;
    const auto status = input();
    if (status != AYNI_OK && status != AYNI_NOT_READY) {
        input_.clear();
        return status;
    }
    result_.reset();
    goal_ = 0;
    end_request_ = EndRequest::none;
    held_ = false;
    phase_ = status == AYNI_OK ? Phase::save : Phase::input;
    if (status == AYNI_OK && config_.persistence == strategies::Persistence::background) {
        current_ = candidate_; // Volatile draft; only a completed durable file publishes history.
        phase_ = Phase::start_turn;
    }
    return AYNI_OK;
}
// Stores the accepted input as a body; NOT_READY until the body store made it durable.
ayni_status_t Session::input() {
    auto bodies = *config_.bodies.store;
    if (config_.persistence == strategies::Persistence::background) {
        bodies.write = bodies.stage;
    }
    const auto status = Conversation::begin(config_.workspace, bodies, current_,
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
ayni_status_t Session::turn(const ayni_context_info_v1 &anchor, uint64_t durable) {
    if (end_request_ != EndRequest::none) {
        const auto status = cancel_turn(durable);
        if (status != AYNI_OK || phase_ != Phase::turn) {
            return status;
        }
    }
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
        } else if (accepted.status != AYNI_BUSY) {
            result_ = TurnResult{0, 0, accepted.status, accepted.error.reason, false};
            return unfinished();
        }
        return accepted.status;
    }
    auto goal = initialized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(config_.kernel, goal_, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    if (!goal.last_task_id) {
        if (goal.commit_seq <= durable &&
            (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED ||
             goal.lifecycle == AYNI_GOAL_EXPIRED)) {
            result_ = TurnResult{goal_, 0, AYNI_NOT_READY, goal.wait_reason, false};
            return unfinished();
        }
        return AYNI_OK;
    }
    auto task = initialized<ayni_task_info_v1>();
    status = ayni_get_task(config_.kernel, goal.last_task_id, &task);
    if (status != AYNI_OK) {
        return status;
    }
    if (task.lifecycle == AYNI_TASK_PAUSED && task.hold_status != AYNI_OK) {
        held_ = true;
        return task.hold_status; // Report the Core hold; poll keeps the original Task attached.
    }
    if (task.commit_seq > durable) {
        return AYNI_OK;
    }
    if (task.lifecycle == AYNI_TASK_COMPLETED) {
        if (config_.persistence == strategies::Persistence::background) {
            auto copied = ayni_copy_task_checkpoint(
                config_.kernel, task.task_id, parameters_.bytes.data(),
                static_cast<uint32_t>(parameters_.bytes.size()), &parameters_.size);
            if (copied != AYNI_OK) {
                return copied;
            }
            copied =
                Conversation::finish(config_.workspace, current_, parameters_.view(), candidate_);
            if (copied == AYNI_OK) {
                copied = confirm(candidate_);
            }
            if (copied != AYNI_OK) {
                return copied == AYNI_NOT_READY ? AYNI_BUSY : copied;
            }
        }
        result_ = TurnResult{goal_, task.task_id, AYNI_OK, AYNI_REASON_NONE, false};
        const auto accepted = command(AYNI_INTENT_CONTEXT_PUBLISH, anchor, task.task_id,
                                      AYNI_CONTEXT_IDLE, anchor.last_turn_id + 1);
        if (accepted.status == AYNI_OK) {
            phase_ = Phase::publish;
        }
        return accepted.status;
    }
    if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_EXPIRED ||
        task.lifecycle == AYNI_TASK_CANCELLED) {
        result_ = TurnResult{goal_, task.task_id, AYNI_NOT_READY, task.hold_reason, false};
        return unfinished(); // Retention remains the default.
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
        result_.reset();
        phase_ = Phase::erase;
    }
    return accepted.status;
}
ayni_status_t Session::erase(const ayni_context_info_v1 &anchor) {
    if (anchor.pending_action < AYNI_CONTEXT_ERASE_ALL) {
        return AYNI_NOT_READY;
    }
    const auto &bodies = *config_.bodies.store;
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
    if (config_.bodies.ready) {
        // Idle Sessions must also advance queued body cleanup after ordinary backpressure.
        static_cast<void>(config_.bodies.ready(config_.bodies.context, 0));
    }
    if (config_.bodies.status) {
        const auto stored = config_.bodies.status(config_.bodies.context);
        if (stored != AYNI_OK) {
            failure_ = stored;
            phase_ = Phase::failed;
        } // An idle failed queue can be disposed, but cannot acknowledge a clean close.
    }
    if (phase_ == Phase::failed) {
        return failure_;
    }
    if (config_.persistence == strategies::Persistence::background && current_.id) {
        const auto stored = confirm(current_);
        if (stored != AYNI_OK && stored != AYNI_NOT_READY) {
            failure_ = stored;
            phase_ = Phase::failed;
            return stored;
        }
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
    held_ = false;
    switch (phase_) {
    case Phase::load:
    case Phase::loading:
        status = load(anchor);
        break;
    case Phase::input:
        status = input();
        if (status == AYNI_OK) {
            phase_ = config_.persistence == strategies::Persistence::background ? Phase::start_turn
                                                                                : Phase::save;
            if (config_.persistence == strategies::Persistence::background) {
                current_ = candidate_;
            }
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
        status = turn(anchor, kernel.durable_seq);
        break;
    case Phase::terminal:
        status = unfinished();
        break;
    case Phase::publish:
        status = publish(anchor);
        break;
    case Phase::erase:
        status = erase(anchor);
        break;
    case Phase::collect:
        status = collect_superseded(*superseded_);
        if (status == AYNI_OK) {
            phase_ = Phase::collecting;
        }
        break;
    case Phase::collecting:
        status = config_.bodies.status ? config_.bodies.status(config_.bodies.context) : AYNI_OK;
        if (status != AYNI_OK) {
            break;
        }
        if (!config_.bodies.ready || config_.bodies.ready(config_.bodies.context, 0)) {
            if (status == AYNI_OK) {
                superseded_.reset();
                phase_ = save_ == Save::discard ? Phase::ready : Phase::acknowledge;
            }
        }
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
            if (save_ == Save::publication) {
                if (result_) {
                    result_->published = true;
                }
            }
            end_request_ = EndRequest::none;
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
    if (status != AYNI_OK && status != AYNI_BUSY && phase_ != Phase::turn &&
        phase_ != Phase::terminal) {
        failure_ = status;
        phase_ = Phase::failed;
    }
    return status;
}
// A discarded saved draft is an explicit interaction policy, never permission to replay.
// The composition must first cancel the old ONCE Goals and wait for their Tasks to terminate.
ayni_status_t Session::settled(uint64_t durable) const {
    uint32_t count = 0;
    auto status = ayni_list_objects(config_.kernel, AYNI_OBJECT_GOAL, nullptr, 0, &count);
    if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    std::vector<uint64_t> ids(count);
    status = ayni_list_objects(config_.kernel, AYNI_OBJECT_GOAL, ids.data(), count, &count);
    if (status != AYNI_OK) {
        return status;
    }
    for (const auto id : ids) {
        auto goal = initialized<ayni_goal_info_v1>();
        status = ayni_get_goal(config_.kernel, id, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.owner_instance_id != config_.owner_instance ||
            goal.goal_type_id_size != config_.goal_type.size ||
            std::memcmp(goal.goal_type_id, config_.goal_type.data, config_.goal_type.size)) {
            continue;
        }
        if (goal.commit_seq > durable || goal.lifecycle == AYNI_GOAL_PROPOSED ||
            goal.lifecycle == AYNI_GOAL_ACTIVE || goal.lifecycle == AYNI_GOAL_PAUSED) {
            return AYNI_NOT_READY;
        }
        if (goal.last_task_id) {
            auto task = initialized<ayni_task_info_v1>();
            status = ayni_get_task(config_.kernel, goal.last_task_id, &task);
            if (status != AYNI_OK) {
                return status;
            }
            if (task.commit_seq > durable ||
                (task.lifecycle != AYNI_TASK_COMPLETED && task.lifecycle != AYNI_TASK_FAILED &&
                 task.lifecycle != AYNI_TASK_CANCELLED && task.lifecycle != AYNI_TASK_EXPIRED)) {
                return AYNI_NOT_READY;
            }
        }
    }
    return AYNI_OK;
}
// File deletion and cache collection have one owner. Loaded bodies may have no cached
// scope, so explicitly release the old record's references after its replacement is durable.
ayni_status_t Session::collect_superseded(const strategies::ConversationRecord &old) {
    std::vector<strategies::BodyRef> before, after;
    auto status = Conversation::references(*config_.bodies.store, old, before);
    if (status == AYNI_OK) {
        status = Conversation::references(*config_.bodies.store, current_, after);
    }
    if (status != AYNI_OK) {
        return status == AYNI_NOT_READY ? AYNI_BUSY : status;
    }
    const auto *store = config_.bodies.store;
    for (const auto &ref : before) {
        bool named = false;
        for (const auto &keep : after) {
            named = named || keep.id == ref.id;
        }
        if (!named) {
            store->release(store->context, &ref);
        }
    }
    config_.bodies.collect(config_.bodies.context, current_.id, after);
    return AYNI_OK;
}
bool Session::active() const {
    switch (phase_) {
    case Phase::input:
    case Phase::start_turn:
    case Phase::turn:
    case Phase::terminal:
    case Phase::publish:
        return true;
    case Phase::failed:
        return goal_ != 0; // Storage failure retains the Goal so the stop entrance stays live.
    case Phase::save:
    case Phase::saving:
        return save_ == Save::draft || save_ == Save::publication || save_ == Save::discard;
    case Phase::collect:
    case Phase::collecting:
    case Phase::acknowledge:
    case Phase::acknowledged:
        return save_ == Save::publication;
    default:
        return false;
    }
}
ayni_status_t Session::cancel() {
    if (phase_ == Phase::failed && goal_) {
        // Cancellation uses Core directly; a failed body store cannot block the stop entrance.
        auto info = initialized<ayni_kernel_info_v1>();
        const auto status = ayni_get_kernel_info(config_.kernel, &info);
        return status == AYNI_OK ? cancel_turn(info.durable_seq) : status;
    }
    if (!active()) {
        return phase_ == Phase::failed ? failure_ : AYNI_INVALID_STATE;
    }
    if (end_request_ == EndRequest::none) {
        end_request_ = EndRequest::cancel;
    }
    return AYNI_OK;
}
ayni_status_t Session::abandon() {
    if (phase_ != Phase::terminal) {
        return AYNI_INVALID_STATE;
    }
    end_request_ = EndRequest::abandon;
    return AYNI_OK;
}
ayni_status_t Session::quiesce() {
    if (phase_ == Phase::empty) {
        return AYNI_OK;
    }
    if (active() && phase_ != Phase::terminal) {
        const auto status = cancel();
        if (status != AYNI_OK) {
            return status;
        }
    }
    const auto status = poll();
    if (status != AYNI_OK && status != AYNI_BUSY && phase_ != Phase::terminal && !held_) {
        return status;
    }
    if (!ready() && phase_ != Phase::terminal) {
        return AYNI_BUSY;
    }
    return !config_.bodies.ready || config_.bodies.ready(config_.bodies.context, 0) ? AYNI_OK
                                                                                    : AYNI_BUSY;
}
bool Session::take_result(TurnResult &out) {
    if ((!ready() && phase_ != Phase::terminal) || !result_) {
        return false;
    }
    out = *result_;
    result_.reset();
    return true;
}
// Discard only the outer draft. Original Core Tasks/Calls, including UNKNOWN evidence,
// retain their existing lifecycle and reclamation rules.
ayni_status_t Session::unfinished() {
    if (config_.unfinished == UnfinishedTurn::discard || end_request_ == EndRequest::abandon) {
        return discard();
    }
    phase_ = Phase::terminal;
    return AYNI_NOT_READY;
}
ayni_status_t Session::discard() {
    candidate_ = current_;
    candidate_.draft = {};
    input_.clear();
    save_ = Save::discard;
    phase_ = Phase::save;
    return AYNI_OK;
}
ayni_status_t Session::cancel_turn(uint64_t durable) {
    if (phase_ == Phase::start_turn) {
        result_ = TurnResult{0, 0, AYNI_NOT_READY, AYNI_REASON_NONE, false};
        return unfinished();
    }
    auto goal = initialized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(config_.kernel, goal_, &goal);
    if (status != AYNI_OK || goal.commit_seq > durable) {
        return status == AYNI_OK ? AYNI_BUSY : status;
    }
    if (goal.lifecycle == AYNI_GOAL_ACTIVE || goal.lifecycle == AYNI_GOAL_PAUSED ||
        goal.lifecycle == AYNI_GOAL_PROPOSED) {
        const ayni_goal_target_v1 target{sizeof(target), 0, goal_};
        return config_.commands
            .submit(config_.commands.context, AYNI_INTENT_GOAL_CANCEL, &target, goal.revision)
            .status;
    }
    return AYNI_OK;
}
} // namespace ayni::runtime::conversation
