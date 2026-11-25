#include "motor.h"
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
namespace wifichat {
namespace {
using Activity = ayni::runtime::OperationSession;
template <class T> T sized() {
    T result{};
    result.struct_size = sizeof(result);
    return result;
}
ayni_status_t post(ayni_provider_completion_t *completion, ayni_attempt_ref_v1 attempt,
                   ayni_status_t code, const char *body) {
    auto result = sized<ayni_provider_result_v1>();
    result.attempt = attempt;
    result.outcome = code == AYNI_OK ? AYNI_CALL_COMPLETED : AYNI_CALL_FAILED;
    result.status = code;
    result.effect = code == AYNI_OK ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_NONE;
    if (body) {
        result.result = motor_text(body);
    }
    return ayni_provider_post(completion, &result);
}
} // namespace
ayni_status_t Motor::configure_session(ayni_kernel_t *kernel, const ayni_workspace_v1 *workspace,
                                       Activity::Commands commands) {
    session_workspace_ = workspace;
    Activity::Config config;
    config.goal_type = motor_text(stream_goal_type);
    config.strategy_id = motor_text("wifi.control.operation");
    config.binding_id = motor_text(stream_execute);
    config.binding_version = motor_version;
    config.policy_context = this;
    config.requirements = session_requirements;
    config.workspace = workspace;
    config.max_lifetime_us = uint64_t(motor_plan_duration_ms_max) * 1000U;
    config.commands = commands;
    return activity_.install(kernel, config);
}
// Independent Goal authority is the exact requested opening envelope/initial values/lifetime.
// Source-turn authorization is never copied, transferred, or promoted into this requirement.
ayni_status_t Motor::session_requirements(void *context, ayni_bytes_v1 bytes,
                                          const ayni_goal_requirement_sink_v1 *sink) {
    auto &self = *static_cast<Motor *>(context);
    uint32_t count = 0;
    auto status = ayni_value_read(self.session_workspace_, bytes, self.session_nodes_.data(),
                                  stream_nodes, &count);
    if (status != AYNI_OK) {
        return status;
    }
    MotorStreamInput input;
    if (!read_motor_stream({self.session_nodes_.data(), count}, input)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::array<ayni_scope_field_v1, stream_field_count> fields{};
    for (uint32_t at = 0; at < fields.size(); ++at) {
        auto &field = fields[at];
        field.struct_size = sizeof(field);
        field.field_id = at + 1;
        field.allowed.struct_size = sizeof(field.allowed);
        field.allowed.kind = AYNI_AUTHORITY_I64;
        field.allowed.unit = motor_text(at + 1 == stream_duration ? "ms" : "percent");
        field.allowed.values.integer = {input.fields[at], input.fields[at]};
    }
    const ayni_goal_requirement_v1 need{
        sizeof(need),
        motor_version,
        motor_text(stream_execute),
        {sizeof(ayni_scope_v1), uint32_t(fields.size()), fields.data()}};
    return sink->emit(sink->ctx, &need);
}
ayni_submit_result_t Motor::open_session(const ayni_provider_request_v1 &request,
                                         ayni_provider_completion_t *completion) {
    MotorStreamInput input;
    if (session_admission_ != SessionAdmission::open || opening_ ||
        activity_.snapshot().phase != Activity::Phase::idle ||
        !read_motor_stream(request.value, input)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto start_seq = transport_.control_mailbox().reserve_start();
    if (!start_seq || transport_.control_mailbox().target_blocked(start_seq)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    opening_ = Opening{request.attempt, completion, request.logical_call_id, input, start_seq};
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t Motor::steer(const ayni_provider_request_v1 &request,
                                  ayni_provider_completion_t *completion) {
    if (session_admission_ != SessionAdmission::open || steering_ || !request.value.nodes ||
        request.value.node_count != steer_input_.size()) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ControlTarget target;
    target.source = ControlTargetSource::authorized_model;
    for (uint32_t at = 1; at < request.value.node_count; ++at) {
        const auto &node = request.value.nodes[at];
        if (node.field_id == 1) {
            target.motor1 = int32_t(node.value.integer);
        } else if (node.field_id == 2) {
            target.motor2 = int32_t(node.value.integer);
        } else if (node.field_id == 3) {
            target.duration_ms = uint32_t(node.value.integer);
        } else if (node.field_id == 4) {
            target.session_goal = node.value.unsigned_integer;
        }
    }
    if (!valid_motor_command({target.motor1, target.motor2, target.duration_ms})) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    steering_ = Steering{request.attempt, completion, request.logical_call_id, 0, target};
    return AYNI_SUBMIT_ACCEPTED;
}

// Owner lifecycle orchestration stays outside Provider callbacks; each step has one borrow owner.
ayni_status_t Motor::advance_sessions(ayni_kernel_t *kernel, uint64_t boot, uint64_t durable) {
    if (session_admission_ == SessionAdmission::closing) {
        close_sessions();
    }
    // A stop closes independently authorized work even before its internal physical Call is
    // dispatched. A later explicit open reserves a newer identity and is admitted normally.
    auto &mailbox = transport_.control_mailbox();
    if (opening_ && mailbox.target_blocked(opening_->start_seq)) {
        opening_->phase = Opening::Phase::rollback;
        activity_.close_all();
    }
    if (active_stream_ && mailbox.target_blocked(active_start_seq_)) {
        activity_.close_all();
    }
    auto status = advance_opening(kernel, boot);
    if (status == AYNI_OK) {
        status = activity_.advance(durable, uint64_t(esp_timer_get_time()));
    }
    if (status != AYNI_OK) {
        activity_.close_all();
        return status;
    }
    status = deliver_opening(activity_.snapshot());
    if (status == AYNI_OK) {
        status = advance_execute(kernel);
    }
    if (status == AYNI_OK) {
        status = advance_steering(kernel);
    }
    return status == AYNI_OK ? settle_operation(activity_.snapshot()) : status;
}
// Retained opening copies exact scalar input and rolls back if its source Call was cancelled.
ayni_status_t Motor::advance_opening(ayni_kernel_t *kernel, uint64_t boot) {
    if (opening_ && opening_->phase == Opening::Phase::queued) {
        auto source = sized<ayni_call_info_v1>();
        const auto queried = ayni_get_call(kernel, opening_->source_call, &source);
        if (queried != AYNI_OK || source.cancel_requested || source.outcome) {
            opening_->phase = Opening::Phase::rollback;
        } else {
            // Reconstruct the copied canonical scalar input; no borrowed Provider tree survives.
            session_nodes_[0] = {};
            session_nodes_[0].kind = AYNI_VALUE_OBJECT;
            session_nodes_[0].subtree_end = stream_nodes;
            for (uint32_t at = 1; at < stream_nodes; ++at) {
                auto &node = session_nodes_[at];
                node = {};
                node.kind = AYNI_VALUE_I64;
                node.field_id = at;
                node.subtree_end = at + 1;
                node.value.integer = opening_->input.fields[at - 1];
            }
            const auto opened = activity_.open(
                boot, source.owner_instance_id, {session_nodes_.data(), stream_nodes},
                uint64_t(opening_->input.initial.duration_ms) * 1000U);
            if (opened != AYNI_OK) {
                const auto delivered =
                    post(opening_->completion, opening_->attempt, opened, nullptr);
                if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
                    opening_.reset();
                }
            } else {
                opening_->phase = Opening::Phase::creating;
            }
        }
    }
    if (opening_ && opening_->phase != Opening::Phase::queued) {
        auto source = sized<ayni_call_info_v1>();
        const auto queried = ayni_get_call(kernel, opening_->source_call, &source);
        if (queried != AYNI_OK || source.cancel_requested || source.outcome ||
            opening_->phase == Opening::Phase::rollback) {
            opening_->phase = Opening::Phase::rollback;
            activity_.close_all();
        }
    }
    return AYNI_OK;
}
// Physical execution remains gated until the independent handle was successfully posted.
ayni_status_t Motor::deliver_opening(const Activity::Snapshot &snapshot) {
    if (opening_ && snapshot.phase == Activity::Phase::ready_to_activate) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "{\"session_id\":%llu,\"boot\":%llu,\"generation\":%llu,\"authorized\":true,"
                      "\"output_applied\":false}",
                      (unsigned long long)snapshot.handle.goal,
                      (unsigned long long)snapshot.handle.boot,
                      (unsigned long long)snapshot.handle.generation);
        const auto delivered = post(opening_->completion, opening_->attempt, AYNI_OK, body);
        if (delivered == AYNI_OK) {
            active_stream_ = opening_->input;
            active_start_seq_ = opening_->start_seq;
            const auto status = activity_.activate(snapshot.handle, uint64_t(esp_timer_get_time()));
            opening_.reset(); // Handle delivery is final even when its finite lifetime just ended.
            if (status != AYNI_OK) {
                activity_.close_all();
                if (status != AYNI_EXPIRED) {
                    return status;
                }
            }
        } else if (delivered == AYNI_INVALID_STATE) {
            opening_->phase = Opening::Phase::rollback;
            activity_.close_all();
        } else if (delivered != AYNI_BUSY) {
            return delivered;
        }
    }
    return AYNI_OK;
}
// Only the operation's original Task may hand off its internal physical Call.
ayni_status_t Motor::advance_execute(ayni_kernel_t *kernel) {
    if (execute_) {
        auto source = sized<ayni_call_info_v1>();
        const auto queried = ayni_get_call(kernel, execute_->call, &source);
        const auto current = activity_.snapshot();
        const bool admitted = execute_->state == Execute::State::queued && queried == AYNI_OK &&
                              !source.cancel_requested && !source.outcome &&
                              current.phase == Activity::Phase::active &&
                              uint64_t(esp_timer_get_time()) < current.expires_at_us &&
                              source.task_id == current.task &&
                              source.owner_instance_id == current.handle.owner;
        if (admitted && execution_.submit_stream(
                            execute_->attempt, execute_->call, execute_->completion,
                            execute_->permit, execute_->initial, current.handle.goal,
                            current.expires_at_us, active_start_seq_) == AYNI_SUBMIT_ACCEPTED) {
            execute_.reset();
        } else {
            const auto delivered =
                post(execute_->completion, execute_->attempt, AYNI_DENIED, nullptr);
            if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
                execute_.reset();
            } else if (delivered != AYNI_BUSY) {
                return delivered;
            }
        }
    }
    return AYNI_OK;
}
// Hot targets need both fresh source authority and the current operation's exact ownership.
ayni_status_t Motor::advance_steering(ayni_kernel_t *kernel) {
    if (steering_) {
        auto &steer = *steering_;
        if (!steer.seq && steer.status == AYNI_OK) {
            auto source = sized<ayni_call_info_v1>();
            const auto queried = ayni_get_call(kernel, steer.source_call, &source);
            const auto current = activity_.snapshot();
            if (queried != AYNI_OK || source.cancel_requested || source.outcome ||
                current.phase != Activity::Phase::active || !active_stream_ ||
                source.owner_instance_id != current.handle.owner ||
                steer.target.session_goal != current.handle.goal) {
                steer.status = AYNI_NOT_READY;
            } else if (!active_stream_->contains(steer.target.motor1, steer.target.motor2) ||
                       steer.target.duration_ms > active_stream_->initial.duration_ms) {
                steer.status = AYNI_DENIED;
            } else if (!transport_.submit_motor_target(steer.target, &steer.seq)) {
                return AYNI_BUSY; // Retained contention retries after the owner's 1 ms busy wait.
            }
        }
        const auto status = complete_steering();
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
// Release only terminal independent work after all outstanding opening/execution borrows drain.
ayni_status_t Motor::settle_operation(const Activity::Snapshot &snapshot) {
    if (snapshot.phase == Activity::Phase::terminal) {
        if (opening_) {
            const auto code = snapshot.status == AYNI_OK ? AYNI_DENIED : snapshot.status;
            const auto delivered = post(opening_->completion, opening_->attempt, code, nullptr);
            if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
                opening_.reset();
            } else if (delivered != AYNI_BUSY) {
                return delivered;
            }
        }
        if (!opening_ && !execute_) {
            active_stream_.reset();
            return activity_.release();
        }
    } else if (opening_ && opening_->phase == Opening::Phase::rollback &&
               snapshot.phase == Activity::Phase::idle) {
        const auto delivered = post(opening_->completion, opening_->attempt, AYNI_DENIED, nullptr);
        if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
            opening_.reset();
        }
    }
    return AYNI_OK;
}
// Once accepted into the target mailbox, retain that receipt even during shutdown.
// Output stop is a separate fact and cannot rewrite the prior accepted sequence.
ayni_status_t Motor::complete_steering() {
    if (!steering_) {
        return AYNI_OK;
    }
    const auto &steer = *steering_;
    char body[160];
    std::snprintf(body, sizeof(body),
                  "{\"accepted\":%s,\"target_seq\":%llu,\"output_applied\":false}",
                  steer.status == AYNI_OK ? "true" : "false", (unsigned long long)steer.seq);
    const auto delivered = post(steer.completion, steer.attempt, steer.status, body);
    if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
        steering_.reset();
        return AYNI_OK;
    }
    return delivered == AYNI_BUSY ? AYNI_OK : delivered;
}
// Closing admission and draining retained callbacks are separate from Goal management.
// Kernel shutdown cancels admitted Calls; no management command or hardware start is issued here.
void Motor::close_sessions() {
    session_admission_ = SessionAdmission::closing;
    cancel_sessions();
}
void Motor::cancel_sessions() {
    activity_.close_all();
    if (opening_) {
        opening_->phase = Opening::Phase::rollback;
    }
    if (execute_) {
        execute_->state = Execute::State::cancelled;
    }
    if (steering_ && !steering_->seq) {
        steering_->status = AYNI_DENIED;
    }
}
ayni_status_t Motor::drain_sessions() {
    close_sessions();
    auto drain = [](auto &slot) -> ayni_status_t {
        if (!slot) {
            return AYNI_OK;
        }
        const auto delivered = post(slot->completion, slot->attempt, AYNI_DENIED, nullptr);
        if (delivered == AYNI_OK || delivered == AYNI_INVALID_STATE) {
            slot.reset();
            return AYNI_OK;
        }
        return delivered == AYNI_BUSY ? AYNI_OK : delivered;
    };
    auto status = drain(opening_);
    if (status == AYNI_OK) {
        status = drain(execute_);
    }
    if (status == AYNI_OK) {
        status = complete_steering();
    }
    return status;
}
} // namespace wifichat
