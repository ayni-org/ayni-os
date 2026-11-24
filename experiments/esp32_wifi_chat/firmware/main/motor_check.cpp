#include "motor_check.h"
#include <cstdio>
namespace wifichat {
namespace {
constexpr const char *types[]{"wifi.motor.check1", "wifi.motor.check2"};
constexpr const char *strategy_id = "wifi.motor.check";
template <class T> T sized() {
    T value{};
    value.struct_size = sizeof(value);
    return value;
}
constexpr size_t motor_offset = sizeof(uint32_t), status_offset = 2 * sizeof(uint32_t),
                 reason_offset = 3 * sizeof(uint32_t);
} // namespace
void MotorCheckNotice::encode(uint8_t *out) const {
    put32(out, uint32_t(phase));
    put32(out + motor_offset, motor);
    put32(out + status_offset, uint32_t(status));
    put32(out + reason_offset, reason);
}
bool MotorCheckNotice::decode(const uint8_t *in, size_t bytes, MotorCheckNotice &out) {
    if (!in || bytes != wire_bytes || get32(in) < uint32_t(Phase::queued) ||
        get32(in) > uint32_t(Phase::cancelled) ||
        get32(in + motor_offset) >= MotorCheck::type_count) {
        return false;
    }
    const auto bits = get32(in + status_offset);
    ayni_status_t code = 0;
    static_assert(sizeof(code) == sizeof(bits));
    std::memcpy(&code, &bits, sizeof(code));
    out = {Phase(get32(in)), get32(in + motor_offset), code, get32(in + reason_offset)};
    return true;
}
ayni_status_t MotorCheck::configure(Owner owner) {
    if (inner_.decide) {
        return AYNI_INVALID_STATE;
    }
    if (!owner.ready || !owner.submit || !owner.notify || !owner.stop) {
        return AYNI_INVALID_ARGUMENT;
    }
    owner_ = owner;
    for (uint32_t motor = 0; motor < type_count; ++motor) {
        auto &p = profiles_[motor];
        p.input[0].kind = AYNI_VALUE_OBJECT;
        p.input[0].subtree_end = p.input.size();
        for (uint32_t at = 0; at < field_count; ++at) {
            auto &f = p.fields[at];
            f.struct_size = sizeof(f);
            f.field_id = at + 1;
            f.allowed.struct_size = sizeof(f.allowed);
            f.allowed.kind = AYNI_AUTHORITY_I64;
            f.allowed.unit = motor_text(at == field_count - 1 ? "ms" : "percent");
            const int64_t value = at == field_count - 1 ? duration_ms : at == motor ? percent : 0;
            f.allowed.values.integer = {value, value};
            auto &node = p.input[at + 1];
            node.kind = AYNI_VALUE_I64;
            node.field_id = at + 1;
            node.subtree_end = at + 2;
            node.value.integer = value;
        }
        p.step.name = motor_text("run");
        p.step.version = version;
        p.step.call.binding_id = motor_text("motor.run");
        p.step.call.binding_version = motor_version;
        p.step.call.preemption = AYNI_CALL_PREEMPT_SUPERSEDE;
        // Explicit native input avoids the scalar template's extra empty text field.
        p.step.call.input = ayni_value_v1{p.input.data(), uint32_t(p.input.size())};
        rules_[motor] = {motor_text(types[motor]), version, &p.step, 1};
    }
    const auto status =
        policy_.configure({motor_text(strategy_id), version}, rules_.data(), type_count);
    if (status == AYNI_OK) {
        inner_ = policy_.describe();
    }
    return status;
}
ayni_strategy_v1 MotorCheck::strategy() {
    auto out = inner_;
    out.ctx = this;
    out.decide = decide;
    out.ready = ready;
    return out;
}
uint32_t MotorCheck::ready(void *context, uint64_t) {
    auto &self = *static_cast<MotorCheck *>(context);
    return self.owner_.ready(self.owner_.context) ? 1U : 0U;
}
ayni_status_t MotorCheck::decide(void *context, const ayni_task_snapshot_v1 *task,
                                 ayni_policy_output_v1 *out) {
    auto &self = *static_cast<MotorCheck *>(context);
    return self.inner_.decide(self.inner_.ctx, task, out);
}
ayni_status_t MotorCheck::validate(void *, ayni_bytes_v1 parameters) {
    return parameters.size ? AYNI_INVALID_ARGUMENT : AYNI_OK;
}
ayni_status_t MotorCheck::requirements(void *context, ayni_bytes_v1,
                                       const ayni_goal_requirement_sink_v1 *sink) {
    const auto &p = *static_cast<const Profile *>(context);
    const ayni_goal_requirement_v1 need{sizeof(need),
                                        motor_version,
                                        motor_text("motor.run"),
                                        {sizeof(ayni_scope_v1), field_count, p.fields.data()}};
    return sink->emit(sink->ctx, &need);
}
ayni_status_t MotorCheck::install(ayni_kernel_t *kernel) {
    if (!inner_.decide) {
        return AYNI_NOT_READY;
    }
    const auto spec = strategy();
    auto status = ayni_register_strategy(kernel, &spec);
    for (uint32_t motor = 0; status == AYNI_OK && motor < type_count; ++motor) {
        ayni_goal_type_v1 type{};
        status = describe_type(motor, type);
        if (status == AYNI_OK) {
            status = ayni_register_goal_type(kernel, &type);
        }
    }
    return status;
}
ayni_status_t MotorCheck::describe_type(uint32_t motor, ayni_goal_type_v1 &out) {
    if (!inner_.decide) {
        return AYNI_NOT_READY;
    }
    if (motor >= type_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto type = sized<ayni_goal_type_v1>();
    type.version = version;
    type.goal_type_id = motor_text(types[motor]);
    type.strategy_id = inner_.strategy_id;
    type.ctx = &profiles_[motor];
    type.validate = validate;
    type.export_requirements = requirements;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.work_profile.work_class = AYNI_WORK_FOREGROUND;
    type.work_profile.recovery = AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT;
    out = type;
    return AYNI_OK;
}
bool MotorCheck::owns(ayni_string_v1 type, uint32_t candidate_version) {
    if (candidate_version != version) {
        return false;
    }
    for (const auto *name : types) {
        if (std::string_view(type.data, type.size) == name) {
            return true;
        }
    }
    return false;
}
ayni_status_t MotorCheck::request(uint32_t motor, uint64_t id) {
    if (!inner_.decide) {
        return AYNI_NOT_READY;
    }
    if (motor >= type_count || !id) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (run_) {
        return AYNI_BUSY;
    }
    run_ = Run{State::creating, motor, id, 0, {MotorCheckNotice::Phase::queued, motor}};
    if (!owner_.ready(owner_.context)) {
        run_->notice.status = AYNI_NOT_READY;
        finish(MotorCheckNotice::Phase::failed);
        return AYNI_OK;
    }
    owner_.notify(owner_.context, id,
                  run_->notice); // Progress may be dropped; terminal is retained.
    return AYNI_OK;
}
void MotorCheck::cancel() {
    if (run_ && run_->state != State::terminal) {
        owner_.stop(owner_.context);
        if (!run_->goal) {
            finish(MotorCheckNotice::Phase::cancelled);
        } else {
            run_->state = State::cancelling;
        }
    }
}
void MotorCheck::fail(ayni_status_t status, ayni_reason_t reason) {
    if (run_->notice.status == AYNI_OK) {
        run_->notice.status = status;
        run_->notice.reason = reason;
    }
    owner_.stop(owner_.context);
    run_->state = State::cancelling; // Cancel before releasing UI; never replay unknown output.
}
void MotorCheck::finish(MotorCheckNotice::Phase phase) {
    run_->notice.phase = phase;
    run_->state = State::terminal;
    publish();
}
void MotorCheck::publish() {
    if (owner_.notify(owner_.context, run_->request, run_->notice)) {
        run_.reset();
    }
}
void MotorCheck::create(uint64_t instance) {
    auto body = sized<ayni_goal_create_v1>();
    body.goal_type_version = version;
    body.goal_type_id = motor_text(types[run_->motor]);
    body.owner_instance_id = instance;
    body.trigger = AYNI_TRIGGER_ONCE;
    const auto result =
        owner_.submit(owner_.context, AYNI_INTENT_GOAL_CREATE, &body, &run_->goal, 0);
    if (result.status == AYNI_OK) {
        run_->state = State::waiting_task;
    } else if (result.status != AYNI_BUSY) {
        run_->notice.status = result.status;
        run_->notice.reason = result.reason;
        finish(MotorCheckNotice::Phase::failed);
    }
}
void MotorCheck::cancel_goal(const ayni_goal_info_v1 &goal) {
    if (goal.lifecycle == AYNI_GOAL_ACTIVE || goal.lifecycle == AYNI_GOAL_PAUSED ||
        goal.lifecycle == AYNI_GOAL_PROPOSED) {
        const ayni_goal_target_v1 target{sizeof(target), 0, goal.goal_id};
        const auto result =
            owner_.submit(owner_.context, AYNI_INTENT_GOAL_CANCEL, &target, nullptr, goal.revision);
        if (result.status != AYNI_OK && result.status != AYNI_BUSY) {
            std::printf("motor check cancel status=%ld reason=%lu; waiting for stop\n",
                        long(result.status), static_cast<unsigned long>(result.reason));
        }
    }
}
void MotorCheck::poll(ayni_kernel_t *kernel, uint64_t durable) {
    auto goal = sized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(kernel, run_->goal, &goal);
    if (status != AYNI_OK) {
        run_->notice.status = status;
        owner_.stop(owner_.context);
        finish(MotorCheckNotice::Phase::failed);
        return;
    }
    if (goal.lifecycle == AYNI_GOAL_PROPOSED && goal.commit_seq <= durable) {
        fail(AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING);
    }
    if (goal.wait_reason == AYNI_REASON_TASK_CAPACITY) {
        fail(AYNI_CAPACITY_EXCEEDED, goal.wait_reason);
    }
    if (run_->state == State::cancelling) {
        cancel_goal(goal);
    }
    if (!goal.last_task_id) {
        if (goal.commit_seq <= durable &&
            (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED)) {
            finish(run_->notice.status == AYNI_OK ? MotorCheckNotice::Phase::cancelled
                                                  : MotorCheckNotice::Phase::failed);
        }
        return;
    }
    auto task = sized<ayni_task_info_v1>();
    status = ayni_get_task(kernel, goal.last_task_id, &task);
    if (status != AYNI_OK) {
        fail(status, AYNI_REASON_NONE);
        return;
    }
    if (task.lifecycle == AYNI_TASK_PAUSED) {
        fail(task.hold_status, task.hold_reason);
        return;
    }
    uint64_t ids[AYNI_CALL_CAPACITY_MAX]{};
    uint32_t count = 0;
    status = ayni_list_objects(kernel, AYNI_OBJECT_CALL, ids, AYNI_CALL_CAPACITY_MAX, &count);
    if (status == AYNI_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            auto call = sized<ayni_call_info_v1>();
            if (ayni_get_call(kernel, ids[at], &call) == AYNI_OK && call.task_id == task.task_id &&
                call.status != AYNI_OK && run_->notice.status == AYNI_OK) {
                run_->notice.status = call.status;
                run_->notice.reason = call.reason;
            }
        }
    }
    const bool ended = task.lifecycle == AYNI_TASK_COMPLETED ||
                       task.lifecycle == AYNI_TASK_CANCELLED ||
                       task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_EXPIRED;
    if (ended && task.commit_seq <= durable) {
        if (task.lifecycle == AYNI_TASK_COMPLETED) {
            finish(MotorCheckNotice::Phase::completed);
        } else {
            owner_.stop(owner_.context);
            if (run_->notice.status == AYNI_OK && task.lifecycle != AYNI_TASK_CANCELLED) {
                run_->notice.status = AYNI_DENIED;
            }
            finish(run_->notice.status == AYNI_OK ? MotorCheckNotice::Phase::cancelled
                                                  : MotorCheckNotice::Phase::failed);
        }
    } else if (!ended && run_->state == State::waiting_task) {
        run_->state = State::running;
        run_->notice.phase = MotorCheckNotice::Phase::running;
        owner_.notify(owner_.context, run_->request, run_->notice);
    }
}
void MotorCheck::tick(ayni_kernel_t *kernel, uint64_t instance, uint64_t durable) {
    if (!run_) {
        return;
    }
    if (run_->state == State::terminal) {
        publish();
        return;
    }
    if (!owner_.ready(owner_.context)) {
        cancel();
    }
    if (!run_ || run_->state == State::terminal) {
        return;
    }
    if (run_->state == State::creating) {
        create(instance);
    } else {
        poll(kernel, durable);
    }
}
} // namespace wifichat
