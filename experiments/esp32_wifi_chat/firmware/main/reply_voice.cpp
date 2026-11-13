#include "reply_voice.h"
#include "protocol.h"
#include "speech_profile.h"
#include <algorithm>
#include <ayni/strategies/builtin.h>
#include <ayni/values.hpp>
#include <cstring>
namespace wifichat {
namespace {
constexpr uint32_t chunk_bytes = SpeechProfile::text_bytes;
// This composition reserves scope 0 for transient reply speech. Conversation and diagnostic
// records have nonzero scopes, so releasing a spoken chunk cannot delete their deduplicated text.
constexpr uint64_t speech_scope = 0;
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
} // namespace
ayni_status_t ReplyVoice::validate(void *, ayni_bytes_v1 bytes) {
    if (!bytes.data || bytes.size != body_reference_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto ref = get_body_reference(bytes.data);
    return ref.id && ref.size && ref.size <= chunk_bytes && !ref.reserved ? AYNI_OK
                                                                          : AYNI_INVALID_ARGUMENT;
}
ayni_status_t ReplyVoice::requirements(void *, ayni_bytes_v1,
                                       const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 need{sizeof(need),
                                        SpeechProfile::capability_version,
                                        text("speaker.speech"),
                                        {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &need);
}
ayni_strategy_v1 ReplyVoice::strategy(const ayni_body_store_v1 *bodies) {
    bodies_ = bodies;
    auto out = ayni::initialized<ayni_strategy_v1>();
    out.version = version;
    out.strategy_id = text(type_id);
    out.max_intents_per_call = 1;
    out.intent_size = sizeof(ayni_policy_intent_v1);
    out.decide = decide;
    out.ctx = this;
    out.ready = [](void *context, uint64_t) -> uint32_t {
        return static_cast<ReplyVoice *>(context)->accepting_ ? 1U : 0U;
    };
    return out;
}
ayni_goal_type_v1 ReplyVoice::goal_type() {
    auto out = ayni::initialized<ayni_goal_type_v1>();
    out.version = version;
    out.goal_type_id = text(type_id);
    out.strategy_id = text(type_id);
    out.validate = validate;
    out.export_requirements = requirements;
    out.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    out.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    out.context_mode = AYNI_GOAL_CONTEXT_AGENT;
    return out;
}
ayni_status_t ReplyVoice::decide(void *context, const ayni_task_snapshot_v1 *snapshot,
                                 ayni_policy_output_v1 *output) {
    if (!context || !snapshot || !output ||
        validate(nullptr, snapshot->goal_parameters) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<ReplyVoice *>(context);
    if (!self.bodies_) {
        return AYNI_NOT_READY;
    }
    const auto ref = get_body_reference(snapshot->goal_parameters.data);
    std::array<uint8_t, chunk_bytes> bytes{};
    const auto loaded = self.bodies_->read(self.bodies_->context, &ref, bytes.data(), ref.size);
    if (loaded == AYNI_NOT_READY || loaded == AYNI_BUSY) {
        output->disposition = AYNI_POLICY_OUTPUT_DEFER;
        output->intent_count = 0;
        return AYNI_OK;
    }
    if (loaded != AYNI_OK) {
        return loaded;
    }
    // Rules uses request_call's immediate bounded copy, so the local value survives no borrow.
    ayni::ValueBuilder<2> input;
    input.object();
    input.text(1, {reinterpret_cast<const char *>(bytes.data()), ref.size});
    input.end();
    ayni::strategies::RuleStep step{text("speak"), version, {}};
    step.call.binding_id = text("speaker.speak_text");
    step.call.binding_version = SpeechProfile::capability_version;
    ayni_value_v1 value{};
    const auto built = input.finish(value);
    if (built != AYNI_OK) {
        return built;
    }
    step.call.input = value;
    const ayni::strategies::Rule rule{text(type_id), version, &step, 1};
    ayni::strategies::Rules rules;
    const auto rc = rules.configure({text(type_id), version}, &rule, 1);
    if (rc != AYNI_OK) {
        return rc;
    }
    const auto delegate = rules.describe();
    return delegate.decide(delegate.ctx, snapshot, output);
}
ayni_status_t ReplyVoice::begin(std::string answer, const ayni_context_info_v1 &source) {
    if (active()) {
        return AYNI_BUSY;
    }
    if (!bodies_ || !source.context_id || !source.usable ||
        source.pending_action != AYNI_CONTEXT_IDLE || answer.empty() || answer.size() > tx_bytes ||
        ayni_validate_utf8({answer.data(), uint32_t(answer.size())}) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    reset();
    const auto rendered = text_.render(answer, tx_bytes);
    if (rendered != AYNI_OK) {
        return rendered;
    }
    source_ =
        Source{source.context_id, source.owner_instance_id, source.generation, source.revision};
    return AYNI_OK;
}
void ReplyVoice::reset() {
    if (body_.id && bodies_) {
        bodies_->release(bodies_->context, &body_);
    }
    body_ = {};
    text_.clear();
    offset_ = 0;
    chunk_.reset();
    source_.reset();
    goal_ = 0;
    cancelling_ = false;
    failure_ = AYNI_OK;
}
ayni_status_t ReplyVoice::advance(ayni_kernel_t *kernel, ayni::CommandClient &commands,
                                  uint64_t session, uint64_t owner, uint64_t durable) {
    if (!active()) {
        return AYNI_OK;
    }
    return goal_ ? poll_goal(kernel, commands, durable)
                 : prepare_chunk(kernel, commands, session, owner);
}
ayni_status_t ReplyVoice::prepare_chunk(ayni_kernel_t *kernel, ayni::CommandClient &commands,
                                        uint64_t session, uint64_t owner) {
    const auto presentation = text_.view();
    if ((cancelling_ && !chunk_) || offset_ == presentation.size()) {
        return failure_;
    }
    if (commands.pending() && !chunk_) {
        return AYNI_BUSY;
    }
    if (!chunk_) {
        chunk_ = uint32_t(speech_chunk_bytes(presentation.substr(offset_), chunk_bytes));
    }
    if (!body_.id) {
        const auto stored = bodies_->write(
            bodies_->context, speech_scope,
            {reinterpret_cast<const uint8_t *>(presentation.data() + offset_), *chunk_}, &body_);
        if (stored == AYNI_NOT_READY || stored == AYNI_BUSY) {
            return AYNI_BUSY;
        }
        if (stored != AYNI_OK) {
            return stored;
        }
    }
    // Every segment must retain the original answer's basis, not capture a newly cleared
    // or replaced Context as fresh authority for the old in-memory text (C-MEM-005).
    if (!cancelling_) {
        auto current = ayni::initialized<ayni_context_info_v1>();
        const auto queried = ayni_get_context(kernel, session, owner, &current);
        if (queried != AYNI_OK || !source_ || !current.usable ||
            current.pending_action != AYNI_CONTEXT_IDLE || current.context_id != source_->id ||
            owner != source_->owner || current.revision != source_->revision ||
            current.generation != source_->generation) {
            failure_ = queried == AYNI_OK ? AYNI_EXPIRED : queried;
            cancelling_ = true;
        }
    }
    // A write already in progress must finish before its private chunk can be removed.
    // Cancellation never creates a speech Goal or a network request after that write.
    if (cancelling_) {
        bodies_->release(bodies_->context, &body_);
        body_ = {};
        chunk_.reset();
        return failure_;
    }
    if (commands.pending()) {
        return AYNI_BUSY;
    }
    auto body = ayni::initialized<ayni_goal_create_v1>();
    body.goal_type_version = version;
    body.goal_type_id = text(type_id);
    body.owner_instance_id = owner;
    body.trigger = AYNI_TRIGGER_ONCE;
    std::array<uint8_t, body_reference_bytes> parameters{};
    put_body_reference(parameters.data(), body_);
    body.parameters = {parameters.data(), uint32_t(parameters.size())};
    const auto accepted = commands.submit(AYNI_INTENT_GOAL_CREATE, &body);
    if (accepted.status == AYNI_OK) {
        goal_ = accepted.result.object.id;
        return AYNI_BUSY;
    }
    return accepted.status;
}
ayni_status_t ReplyVoice::poll_goal(ayni_kernel_t *kernel, ayni::CommandClient &commands,
                                    uint64_t durable) {
    auto goal = ayni::initialized<ayni_goal_info_v1>();
    auto rc = ayni_get_goal(kernel, goal_, &goal);
    if (rc != AYNI_OK) {
        if (failure_ == AYNI_OK) {
            failure_ = rc;
        }
        cancelling_ = true;
        return AYNI_BUSY; // Query failure is not proof that the Provider borrow ended.
    }
    if (goal.commit_seq <= durable &&
        (goal.lifecycle == AYNI_GOAL_PROPOSED || goal.wait_reason == AYNI_REASON_TASK_CAPACITY)) {
        failure_ = goal.lifecycle == AYNI_GOAL_PROPOSED ? AYNI_DENIED : AYNI_CAPACITY_EXCEEDED;
        cancelling_ = true;
    }
    bool task_settled = goal.last_task_id == 0;
    if (goal.last_task_id) {
        auto task = ayni::initialized<ayni_task_info_v1>();
        rc = ayni_get_task(kernel, goal.last_task_id, &task);
        if (rc != AYNI_OK) {
            if (failure_ == AYNI_OK) {
                failure_ = rc;
            }
            cancelling_ = true;
            return AYNI_BUSY;
        }
        if (task.lifecycle == AYNI_TASK_PAUSED) {
            if (failure_ == AYNI_OK) {
                failure_ = task.hold_status;
            }
            cancelling_ = true;
        }
        task_settled =
            task.commit_seq <= durable &&
            (task.lifecycle == AYNI_TASK_COMPLETED || task.lifecycle == AYNI_TASK_CANCELLED ||
             task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_EXPIRED);
    }
    const bool terminal = goal.lifecycle == AYNI_GOAL_SATISFIED ||
                          goal.lifecycle == AYNI_GOAL_CANCELLED ||
                          goal.lifecycle == AYNI_GOAL_FAILED;
    // Goal cancellation precedes asynchronous Task/Provider termination. Keep the UI turn and
    // body until both the Goal and Task termination are durable, never just cancellation receipt.
    if (terminal && goal.commit_seq <= durable && task_settled) {
        const bool completed = goal.lifecycle == AYNI_GOAL_SATISFIED;
        goal_ = 0;
        bodies_->release(bodies_->context, &body_);
        body_ = {};
        if (cancelling_) {
            return failure_;
        }
        if (!completed) {
            return AYNI_INTERNAL;
        }
        offset_ += *chunk_;
        chunk_.reset();
        return offset_ == text_.view().size() ? AYNI_OK : AYNI_BUSY;
    }
    if (cancelling_ && !commands.pending() && !terminal) {
        const ayni_goal_target_v1 body{sizeof(body), 0, goal_};
        const auto cancelled = commands.submit(AYNI_INTENT_GOAL_CANCEL, &body, goal.revision);
        if (cancelled.status != AYNI_OK && cancelled.status != AYNI_BUSY) {
            return cancelled.status;
        }
    }
    return AYNI_BUSY;
}
} // namespace wifichat
