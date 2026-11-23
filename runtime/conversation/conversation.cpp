#include <algorithm>
#include <ayni/memory.h>
#include <ayni/runtime/conversation.h>
#include <cstring>
namespace ayni::runtime::conversation {
using hosts::storage::FileValueRequest;
using strategies::Conversation;
namespace {
bool same_reference(const strategies::BodyRef &a, const strategies::BodyRef &b) {
    return a.id == b.id && a.digest == b.digest && a.size == b.size && a.reserved == b.reserved;
}
// One immutable identity can be shared by many history entries. Confirm/release it
// once, and reject conflicting metadata before dedup can hide a malformed graph.
ayni_status_t unique_references(std::vector<strategies::BodyRef> &refs) {
    std::sort(refs.begin(), refs.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    for (size_t index = 1; index < refs.size(); ++index) {
        if (refs[index - 1U].id == refs[index].id &&
            !same_reference(refs[index - 1U], refs[index])) {
            return AYNI_STORAGE_FAILED;
        }
    }
    refs.erase(std::unique(refs.begin(), refs.end(), same_reference), refs.end());
    return AYNI_OK;
}
} // namespace
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
    if (phase_ != Phase::ready && phase_ != Phase::terminal) {
        return AYNI_BUSY;
    }
    return current_context(anchor);
}
// File source identity and the live privacy barrier are shared by intake and forget.
// Forget may cancel the original pending turn; intake cannot admit overlapping work.
ayni_status_t Session::current_context(ayni_context_info_v1 &anchor) const {
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
    return phase_ == Phase::ready && intake(anchor) == AYNI_OK;
}
ayni_status_t Session::configure(Config config) {
    if (phase_ != Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    if (!config.kernel || !config.session || !config.owner_instance || !config.goal_type.data ||
        !config.goal_type.size || !config.workspace || !config.files.submit ||
        !config.bodies.store || !config.bodies.collect || !config.commands.submit ||
        ((config.bodies.ready || config.bodies.progress) && !config.bodies.collected) ||
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
    if (!anchor.context_id || anchor.owner_instance_id != config.owner_instance) {
        return AYNI_DENIED;
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
void Session::observe_publication(uint64_t task, PublicationStage stage) const {
    if (config_.observer.on_stage) {
        config_.observer.on_stage(config_.observer.context, goal_, task, stage);
    }
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
        request.role = hosts::storage::FileValueRole::conversation_root;
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
    if (current_.id != context_id_ || current_.owner_instance_id != config_.owner_instance ||
        current_.generation > anchor.revision || current_.revision > anchor.generation ||
        current_.last_round > anchor.last_turn_id) {
        return AYNI_DENIED;
    }
    if (anchor.pending_action >= AYNI_CONTEXT_ERASE_ALL && current_.id == anchor.context_id &&
        current_.owner_instance_id == anchor.owner_instance_id &&
        current_.generation <= anchor.revision && current_.revision <= anchor.generation) {
        const auto restored = restore_turn();
        if (restored != AYNI_OK && restored != AYNI_NOT_FOUND) {
            return restored;
        }
        phase_ = Phase::erase;
        return AYNI_OK;
    }
    if (anchor.pending_action == AYNI_CONTEXT_PUBLISH && !anchor.usable) {
        phase_ = Phase::ready; // Next poll requests derived erasure; no old answer is exposed.
        return AYNI_OK;
    }
    if (anchor.pending_action == AYNI_CONTEXT_PUBLISH) {
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
    const auto restored = restore_turn();
    if (restored != AYNI_NOT_FOUND) {
        return restored;
    }
    if (current_.draft.size) {
        if (config_.unfinished != UnfinishedTurn::discard) {
            phase_ = Phase::terminal;
            result_ = TurnResult{0, 0, AYNI_NOT_READY, AYNI_REASON_NONE, false};
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
// Normal reopen reattaches an already accepted Core Goal by exact source identity.
// No Goal/Call is submitted here; Core alone owns replay, holds and UNKNOWN outcomes.
ayni_status_t Session::restore_turn() {
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
    uint64_t found = 0;
    strategies::Encoded parameters;
    strategies::BodyRef input{};
    for (const auto id : ids) {
        auto goal = initialized<ayni_goal_info_v1>();
        status = ayni_get_goal(config_.kernel, id, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.owner_instance_id != config_.owner_instance ||
            goal.goal_type_id_size != config_.goal_type.size ||
            std::memcmp(goal.goal_type_id, config_.goal_type.data, config_.goal_type.size) ||
            goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED ||
            goal.lifecycle == AYNI_GOAL_EXPIRED) {
            continue;
        }
        status = ayni_get_goal_parameters(config_.kernel, id, parameters.bytes.data(),
                                          parameters.bytes.size(), &parameters.size);
        // Privacy may retire an old completed Goal's parameters. It cannot be this
        // baseline's pending work once that source is gone; never reconstruct or replay it.
        if (status == AYNI_EXPIRED && goal.lifecycle == AYNI_GOAL_SATISFIED) {
            continue;
        }
        if (status == AYNI_OK) {
            status =
                Conversation::pending_input(config_.workspace, current_, parameters.view(), input);
        }
        if (status == AYNI_NOT_FOUND) {
            continue;
        }
        if (status != AYNI_OK || found) {
            return status != AYNI_OK ? status : AYNI_REVISION_CONFLICT;
        }
        found = id;
        parameters_ = parameters;
        current_.draft = input;
    }
    if (!found) {
        return AYNI_NOT_FOUND;
    }
    goal_ = found;
    phase_ = Phase::turn;
    return AYNI_OK;
}
bool Session::confirmation_matches(const strategies::ConversationRecord &record) const {
    if (!confirmed_ || confirmed_->generation != record.generation ||
        confirmed_->revision != record.revision) {
        return false;
    }
    const std::array<strategies::BodyRef, 4> roots{record.summary, record.history, record.draft,
                                                   record.rounds};
    return std::equal(roots.begin(), roots.end(), confirmed_->roots.begin(), same_reference);
}
// Own one immutable dependency set through asynchronous confirmation. A successful
// reference leaves the pending suffix permanently; I/O failures still arrive through the store's
// status.
ayni_status_t Session::confirm(const strategies::ConversationRecord &record) {
    const auto *store = config_.bodies.store;
    if (!store->confirm) {
        return AYNI_OK;
    } // A synchronous write-only store has already crossed the durable boundary.
    if (!confirmation_matches(record)) {
        Confirmation next{record.generation,
                          record.revision,
                          {record.summary, record.history, record.draft, record.rounds},
                          {},
                          0};
        const auto status = Conversation::references(*store, record, next.refs);
        if (status != AYNI_OK) {
            return status;
        }
        const auto unique = unique_references(next.refs);
        if (unique != AYNI_OK) {
            return unique;
        }
        confirmed_ = std::move(next);
    }
    auto &progress = *confirmed_;
    for (size_t index = progress.durable; index < progress.refs.size(); ++index) {
        const auto status = store->confirm(store->context, &progress.refs[index]);
        if (status == AYNI_OK) {
            // Keep each success even if a later reference returns ordinary queue BUSY.
            std::swap(progress.refs[progress.durable++], progress.refs[index]);
        } else if (status != AYNI_NOT_READY) {
            return status;
        }
    }
    return progress.durable == progress.refs.size() ? AYNI_OK : AYNI_NOT_READY;
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
    request.role = hosts::storage::FileValueRole::conversation_root;
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
    if (anchor.pending_action >= AYNI_CONTEXT_ERASE_ALL && save_ != Save::erasure) {
        // Complete the worker borrow, then replace stale bytes under the new privacy revision.
        // The stale candidate is never adopted or acknowledged; collect(scope) removes its tails.
        phase_ = Phase::erase;
        return AYNI_OK;
    }
    if (!matches(candidate_, anchor)) {
        // Privacy may have changed while I/O was in flight. Withhold all text and
        // stop this application; the still-pending Core action is never falsely ACKed.
        return AYNI_NOT_READY;
    }
    const auto previous = current_;
    current_ = candidate_;
    if (save_ == Save::publication || save_ == Save::erasure || save_ == Save::retained ||
        save_ == Save::discard) {
        superseded_ = previous;
    }
    phase_ = save_ == Save::draft                                  ? Phase::start_turn
             : save_ == Save::retained                             ? Phase::terminal
             : save_ == Save::initialize || save_ == Save::discard ? Phase::ready
                                                                   : Phase::acknowledge;
    if (superseded_) {
        phase_ = Phase::collect;
    }
    if (save_ == Save::discard) {
        end_request_ = EndRequest::none;
    }
    if (save_ == Save::publication) {
        observe_publication(result_ ? result_->task : anchor.pending_task_id,
                            PublicationStage::record_saved);
    }
    return AYNI_OK;
}
ayni_status_t Session::begin(ayni_string_v1 user, uint64_t memory_id) {
    auto anchor = initialized<ayni_context_info_v1>();
    const auto available = intake(anchor);
    if (available != AYNI_OK) {
        return available;
    }
    if (phase_ == Phase::terminal && config_.unfinished != UnfinishedTurn::retain_and_append) {
        return AYNI_BUSY;
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
// Stage the input once when supported; the save phase owns durable confirmation before Goal
// admission. Background publication still waits for the full graph to become durable.
ayni_status_t Session::input() {
    auto bodies = *config_.bodies.store;
    if (bodies.stage && bodies.confirm) {
        bodies.write = bodies.stage;
    }
    strategies::BodyRef input{};
    const auto status = Conversation::begin(config_.workspace, bodies, current_,
                                            {input_.data(), static_cast<uint32_t>(input_.size())},
                                            parameters_, memory_id_, &input);
    if (status != AYNI_OK) {
        return status;
    }
    candidate_ = current_;
    candidate_.draft = input;
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
        // begin() already encoded the exact input before starting this Goal.
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
            if (copied != AYNI_OK) {
                return copied;
            }
            result_ = TurnResult{goal_, task.task_id, AYNI_OK, AYNI_REASON_NONE, false};
            phase_ = Phase::prepare_publication;
            observe_publication(task.task_id, PublicationStage::checkpoint_ready);
            return AYNI_OK;
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
// The completed Task checkpoint is immutable. Confirm and publish that same candidate
// without recopying it on each NOT_READY or after the Core publication becomes durable.
ayni_status_t Session::prepare_publication(const ayni_context_info_v1 &anchor) {
    const auto status = confirm(candidate_);
    if (status != AYNI_OK) {
        return status == AYNI_NOT_READY ? AYNI_BUSY : status;
    }
    const auto accepted = command(AYNI_INTENT_CONTEXT_PUBLISH, anchor, result_->task,
                                  AYNI_CONTEXT_IDLE, candidate_.last_round);
    if (accepted.status == AYNI_OK) {
        phase_ = Phase::publishing;
    }
    return accepted.status;
}
ayni_status_t Session::publish(const ayni_context_info_v1 &anchor) {
    if (anchor.pending_action != AYNI_CONTEXT_PUBLISH || !anchor.pending_task_id) {
        return AYNI_NOT_READY;
    }
    const bool direct = phase_ == Phase::publish;
    if (direct) {
        if (publish_observed_task_ != anchor.pending_task_id) {
            publish_observed_task_ = anchor.pending_task_id;
            observe_publication(anchor.pending_task_id, PublicationStage::publish_durable);
        }
        // Normal reopen has no in-memory candidate; reconstruct it once from the
        // completed durable checkpoint, never resume the Task (C-SDK-009).
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
    } else if (!result_ || result_->task != anchor.pending_task_id) {
        return AYNI_REVISION_CONFLICT;
    }
    if (!matches(candidate_, anchor)) {
        return AYNI_REVISION_CONFLICT;
    }
    save_ = Save::publication;
    phase_ = Phase::save;
    observe_publication(anchor.pending_task_id, direct ? PublicationStage::checkpoint_ready
                                                       : PublicationStage::publish_durable);
    return AYNI_OK;
}
ayni_status_t Session::forget(ayni_context_action_t action, uint64_t turn_id) {
    auto anchor = initialized<ayni_context_info_v1>();
    const auto queried = phase_ == Phase::turn ? current_context(anchor) : intake(anchor);
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
    auto kernel = initialized<ayni_kernel_info_v1>();
    auto stopped = ayni_get_kernel_info(config_.kernel, &kernel);
    if (stopped != AYNI_OK) {
        return stopped;
    }
    if (goal_) {
        stopped = cancel_turn(kernel.durable_seq);
        if (stopped != AYNI_OK) {
            return stopped == AYNI_BUSY ? AYNI_OK : stopped;
        }
    }
    stopped = settled(kernel.durable_seq);
    if (stopped != AYNI_OK) {
        return stopped == AYNI_NOT_READY ? AYNI_OK : stopped;
    }
    auto base = current_;
    if (anchor.last_turn_id > base.last_round) {
        stopped = Conversation::retain_published_input(config_.workspace, *config_.bodies.store,
                                                       current_, anchor.last_turn_id, base);
        if (stopped != AYNI_OK) {
            return stopped == AYNI_NOT_READY ? AYNI_OK : stopped;
        }
    }
    result_.reset();
    const auto &bodies = *config_.bodies.store;
    const auto status =
        anchor.pending_action == AYNI_CONTEXT_ERASE_ALL
            ? Conversation::erase(config_.workspace, base, candidate_)
        : anchor.pending_action == AYNI_CONTEXT_ERASE_ROUND
            ? Conversation::erase_round(config_.workspace, bodies, base, anchor.pending_turn_id,
                                        candidate_)
            : Conversation::erase_derived(config_.workspace, bodies, base, candidate_);
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
ayni_status_t Session::poll() {
    return poll_step(nullptr);
}
// Each invocation performs one queue operation or one public state transition.
// No owner text escapes until both the file and the exact Core ACK are durable.
ayni_status_t Session::poll_step(bool *again) {
    if (again) {
        *again = false;
    }
    if (config_.bodies.progress) {
        // Idle Sessions must also advance queued body cleanup after ordinary backpressure.
        static_cast<void>(config_.bodies.progress(config_.bodies.context, 0));
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
    // Only the active draft needs ongoing persistence requests. Once a completed
    // candidate owns confirmation, do not alternate back to the superseded draft graph.
    if (config_.persistence == strategies::Persistence::background && current_.id &&
        (phase_ == Phase::start_turn || phase_ == Phase::turn)) {
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
    if (current_.id == context_id_ && !anchor.usable &&
        anchor.pending_action < AYNI_CONTEXT_ERASE_ALL && phase_ != Phase::load &&
        phase_ != Phase::loading && phase_ != Phase::saving) {
        status = command(AYNI_INTENT_CONTEXT_FORGET, anchor, 0, AYNI_CONTEXT_ERASE_DERIVED).status;
        if (status == AYNI_OK) {
            phase_ = Phase::erase;
        }
        return status;
    }
    const bool saving_erasure =
        save_ == Save::erasure && (phase_ == Phase::save || phase_ == Phase::saving ||
                                   phase_ == Phase::collect || phase_ == Phase::collecting ||
                                   phase_ == Phase::acknowledge || phase_ == Phase::acknowledged);
    if (current_.id == context_id_ && anchor.pending_action >= AYNI_CONTEXT_ERASE_ALL &&
        !saving_erasure && phase_ != Phase::load && phase_ != Phase::loading &&
        phase_ != Phase::saving) {
        phase_ = Phase::erase;
    }
    const auto previous = phase_;
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
        status = end_request_ == EndRequest::abandon ? discard() : AYNI_NOT_READY;
        break;
    case Phase::prepare_publication:
        status = prepare_publication(anchor);
        break;
    case Phase::publishing:
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
        status = config_.bodies.collected
                     ? config_.bodies.collected(config_.bodies.context, current_.id)
                     : AYNI_OK;
        if (status == AYNI_OK) {
            superseded_.reset();
            phase_ = save_ == Save::discard    ? Phase::ready
                     : save_ == Save::retained ? Phase::terminal
                                               : Phase::acknowledge;
            if (save_ == Save::publication) {
                observe_publication(result_ ? result_->task : anchor.pending_task_id,
                                    PublicationStage::collected);
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
                if (save_ == Save::publication && config_.observer.on_ack) {
                    config_.observer.on_ack(config_.observer.context, PublicationAck::submitted, 0);
                }
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
                if (config_.observer.on_ack) {
                    config_.observer.on_ack(config_.observer.context, PublicationAck::durable,
                                            anchor.commit_seq);
                }
                publish_observed_task_ = 0;
                observe_publication(result_ ? result_->task : 0, PublicationStage::context_ready);
            }
            end_request_ = EndRequest::none;
        }
        break;
    case Phase::ready:
        if (anchor.pending_action >= AYNI_CONTEXT_ERASE_ALL) {
            phase_ = Phase::erase;
        }
        break;
    default:
        status = AYNI_INVALID_STATE;
        break;
    }
    if (again && status == AYNI_OK && phase_ != previous) {
        // A new local transition gets one retry. An unchanged phase waits for its actual
        // completion, capacity or durable prefix instead of polling the owner busy.
        *again =
            phase_ == Phase::collect || phase_ == Phase::collecting || phase_ == Phase::acknowledge;
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
    std::vector<strategies::BodyRef> before, discovered;
    auto status = Conversation::references(*config_.bodies.store, old, before);
    const auto *after = &discovered;
    if (status == AYNI_OK) {
        if (confirmation_matches(current_) && confirmed_->durable == confirmed_->refs.size()) {
            after = &confirmed_->refs; // The exact complete graph already owns durable proof.
        } else {
            status = Conversation::references(*config_.bodies.store, current_, discovered);
        }
    }
    if (status == AYNI_OK) {
        status = unique_references(before);
    }
    if (status != AYNI_OK) {
        return status == AYNI_NOT_READY ? AYNI_BUSY : status;
    }
    std::vector<uint64_t> retained;
    retained.reserve(after->size());
    for (const auto &ref : *after) {
        retained.push_back(ref.id);
    }
    std::sort(retained.begin(), retained.end());
    const auto *store = config_.bodies.store;
    for (const auto &ref : before) {
        if (!std::binary_search(retained.begin(), retained.end(), ref.id)) {
            store->release(store->context, &ref);
        }
    }
    config_.bodies.collect(config_.bodies.context, current_.id, *after);
    return AYNI_OK;
}
bool Session::active() const {
    switch (phase_) {
    case Phase::input:
    case Phase::start_turn:
    case Phase::turn:
    case Phase::terminal:
    case Phase::prepare_publication:
    case Phase::publishing:
    case Phase::publish:
        return true;
    case Phase::failed:
        return goal_ != 0; // Storage failure retains the Goal so the stop entrance stays live.
    case Phase::save:
    case Phase::saving:
        return save_ == Save::draft || save_ == Save::publication || save_ == Save::retained ||
               save_ == Save::discard;
    case Phase::collect:
    case Phase::collecting:
    case Phase::acknowledge:
    case Phase::acknowledged:
        return save_ == Save::publication || save_ == Save::retained;
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
    if (phase_ == Phase::turn) {
        // Apply cancellation before the composition's next Provider advance can act on
        // queued work. Backpressure keeps the request pending; terminal facts still drain
        // through turn(), and effects already dispatched are never reported as undone.
        auto info = initialized<ayni_kernel_info_v1>();
        const auto queried = ayni_get_kernel_info(config_.kernel, &info);
        const auto status = queried == AYNI_OK ? cancel_turn(info.durable_seq) : queried;
        return status == AYNI_BUSY ? AYNI_OK : status;
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
    if (config_.unfinished != UnfinishedTurn::discard && phase_ == Phase::turn) {
        const auto status =
            poll(); // Finish already-completed work; never leave a publication half done.
        if (phase_ == Phase::turn && (status == AYNI_OK || held_)) {
            return !config_.bodies.ready || config_.bodies.ready(config_.bodies.context, 0)
                       ? AYNI_OK
                       : AYNI_BUSY;
        }
        if (status != AYNI_OK && status != AYNI_BUSY && !held_) {
            return status;
        }
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
const TurnResult *Session::pending_result() const {
    if ((!ready() && phase_ != Phase::terminal) || !result_) {
        return nullptr;
    }
    return &*result_;
}
bool Session::take_result(TurnResult &out) {
    const auto *pending = pending_result();
    if (!pending) {
        return false;
    }
    out = *pending;
    result_.reset();
    return true;
}
// Discard only the outer draft. Original Core Tasks/Calls, including UNKNOWN evidence,
// retain their existing lifecycle and reclamation rules.
ayni_status_t Session::unfinished() {
    if (config_.unfinished == UnfinishedTurn::discard || end_request_ == EndRequest::abandon) {
        return discard();
    }
    if (config_.persistence == strategies::Persistence::background && current_.draft.id) {
        // A cancelled turn retains its input, never its partial generated tail. The
        // same save/collect pipeline makes the retained draft durable before exposing it.
        candidate_ = current_;
        save_ = Save::retained;
        phase_ = Phase::save;
        return AYNI_OK;
    }
    // The durable draft is already saved. Retire the cancelled turn's unreferenced
    // chat tails through the same collection barrier before exposing retained input.
    superseded_ = current_;
    save_ = Save::retained;
    phase_ = Phase::collect;
    return AYNI_OK;
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
