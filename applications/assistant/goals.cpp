#include "policy_support.h"
#include <cstdio>
#include <new>
namespace ayni::applications::assistant {
using namespace detail;
namespace {
ayni_status_t reminder_body(const Parameters &p, const ayni_task_snapshot_v1 &snapshot,
                            std::string &out) {
    ayni_calendar_civil_v1 civil{};
    civil.struct_size = sizeof(civil);
    const auto status = ayni_calendar_split_time(snapshot.calendar_local_s, &civil);
    if (status != AYNI_OK) {
        return status;
    }
    constexpr size_t date_bytes = 32;
    char date[date_bytes]{};
    const auto written =
        std::snprintf(date, date_bytes, "%04d-%02d-%02d %02d:%02d:%02d", civil.year, civil.month,
                      civil.day, civil.hour, civil.minute, civil.second);
    if (written < 0 || static_cast<size_t>(written) >= date_bytes) {
        return AYNI_INTERNAL;
    }
    out = p.text + "（原定本地时间：" + date + "）";
    return AYNI_OK;
}
} // namespace
ayni_status_t Goals::validate(void *context, ayni_bytes_v1 bytes) {
    try {
        Parameters p;
        const auto status = decode(bytes, p);
        return status == AYNI_OK && p.kind != static_cast<TypeContext *>(context)->kind
                   ? AYNI_INVALID_ARGUMENT
                   : status;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
}
ayni_status_t Goals::requirements(void *context, ayni_bytes_v1 bytes,
                                  const ayni_goal_requirement_sink_v1 *sink) {
    try {
        Parameters p;
        const auto status = decode(bytes, p);
        if (status != AYNI_OK) {
            return status;
        }
        const auto &c = static_cast<const TypeContext *>(context)->owner->config_;
        const auto emit = [&](ayni_string_v1 cap, ayni_scope_v1 scope) {
            const ayni_goal_requirement_v1 r{sizeof(r), 1, cap, scope};
            return sink->emit(sink->ctx, &r);
        };
        if (p.kind == GoalKind::reminder) {
            return emit(c.present_capability, empty_scope());
        }
        if (p.kind == GoalKind::device_check) {
            return emit(c.health_capability, empty_scope());
        }
        const auto model = emit(c.model_capability, c.model_scope);
        return model == AYNI_OK && p.kind == GoalKind::scene_observe
                   ? emit(c.observe_capability, empty_scope())
                   : model;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
}
ayni_status_t Goals::register_all(ayni_kernel_t *kernel, PolicyConfig config) {
    if (configured_) {
        return AYNI_INVALID_STATE;
    }
    configured_ = true;
    config_ = config;
    ayni_strategy_v1 strategy{};
    strategy.struct_size = sizeof(strategy);
    strategy.version = 1;
    strategy.strategy_id = text("assistant.goals");
    strategy.ctx = this;
    strategy.decide = decide;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = 64;
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    for (const auto kind : {GoalKind::reminder, GoalKind::device_check, GoalKind::memory_organize,
                            GoalKind::scene_observe}) {
        ayni_goal_type_v1 type{};
        type.struct_size = sizeof(type);
        type.version = 1;
        type.goal_type_id = text(goal_type(kind));
        auto &type_context = types_[static_cast<size_t>(kind)];
        type_context = {this, kind};
        type.ctx = &type_context;
        type.validate = validate;
        type.export_requirements = requirements;
        type.strategy_id = strategy.strategy_id;
        type.occurrence_wait_max_us = AYNI_RESOURCE_WAIT_DEFAULT_US;
        if (kind == GoalKind::reminder) {
            type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_ONCE) |
                                 AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_DAILY) |
                                 AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_WEEKLY);
            type.work_profile.work_class = AYNI_WORK_REMINDER;
            type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
        } else if (kind == GoalKind::scene_observe) {
            type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
        } else {
            type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL);
            if (kind == GoalKind::memory_organize) {
                type.work_profile.work_class = AYNI_WORK_BACKGROUND;
            }
        }
        status = ayni_register_goal_type(kernel, &type);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
// Call facts are the progress ledger. Reminder failure holds pending work instead of completing it.
ayni_status_t Goals::decide(void *context, const ayni_task_snapshot_v1 *snapshot,
                            ayni_policy_output_v1 *output) {
    if (!context || !snapshot || !output) {
        return AYNI_INVALID_ARGUMENT;
    }
    try {
        Parameters p;
        auto status = decode(snapshot->goal_parameters, p);
        if (status != AYNI_OK) {
            return status;
        }
        if (std::strcmp(
                goal_type(p.kind),
                std::string(snapshot->goal_type_id.data, snapshot->goal_type_id.size).c_str()) !=
            0) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto &config = static_cast<const Goals *>(context)->config_;
        if (p.kind == GoalKind::memory_organize) {
            return organize(p, config, *snapshot, *output);
        }
        if (p.kind == GoalKind::scene_observe) {
            return observe(config, *snapshot, *output);
        }
        const bool reminder = p.kind == GoalKind::reminder;
        const auto key = text(reminder ? "notification.v1" : "health.v1");
        ayni_task_call_result_v1 call{};
        status = result(*snapshot, key, call);
        if (status == AYNI_NOT_FOUND) {
            std::string body;
            if (reminder) {
                status = reminder_body(p, *snapshot, body);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            return request(*output, reminder ? "notify" : "health", key, text(body), empty_scope());
        }
        if (!reminder && confirmed_cancel(call)) {
            return report(*snapshot, *output, AYNI_POLICY_REPORT_FAILURE);
        }
        if (reminder && status == AYNI_OK && call.effect != AYNI_CALL_EFFECT_COMPLETE) {
            return AYNI_INVALID_STATE;
        }
        return status == AYNI_OK ? report(*snapshot, *output, AYNI_POLICY_REPORT_COMPLETION)
                                 : status;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
}
} // namespace ayni::applications::assistant
