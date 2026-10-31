#include "suite.h"
#include <ayni/authority.h>
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <ayni/memory.h>
#include <ayni/model_messages.hpp>
#include <ayni/resources.h>
#include <ayni/scenes.h>
#include <cstdio>
#include <cstring>
namespace ayni::portability {
using runtime::portable::agent_count;
using runtime::portable::Profile;
static ayni_status_t sample_resources(Profile &, Evidence &);
namespace {
constexpr uint64_t wait_us = 30000000;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
template <class Done> ayni_status_t until(Profile &profile, Clock clock, Done done) {
    const auto started = clock.now_us(clock.context);
    while (!done()) {
        auto status = profile.tick();
        if (status == AYNI_OK && clock.observe) {
            status = sample_resources(profile, *clock.observe);
        }
        if (status != AYNI_OK) {
            std::printf("tick failed %ld\n", static_cast<long>(status));
            return status;
        }
        if (clock.now_us(clock.context) - started >= wait_us) {
            std::puts("normal case deadline expired");
            return AYNI_EXPIRED;
        }
        clock.yield(clock.context);
    }
    return AYNI_OK;
}
ayni_status_t durable(Profile &p, Clock c) {
    return until(p, c, [&] { return !p.command_pending(); });
}
ayni_status_t submit(Profile &p, Clock c, ayni_intent_kind_t kind, const void *body,
                     uint32_t revision = 0, uint64_t *id = nullptr) {
    auto status = durable(p, c);
    if (status != AYNI_OK) {
        return status;
    }
    const auto accepted = p.submit(kind, body, revision);
    if (accepted.status != AYNI_OK) {
        return accepted.status;
    }
    if (id) {
        *id = accepted.result.object.id;
    }
    return durable(p, c);
}
ayni_status_t conversations(Profile &p, Clock c) {
    ayni_status_t result = AYNI_BUSY;
    const auto status = until(p, c, [&] {
        result = p.open_conversations();
        return result != AYNI_BUSY;
    });
    return status == AYNI_OK ? result : status;
}
} // namespace
// C-WORK/C-AUTH normal application registration uses only public descriptors.
static ayni_status_t done(void *, const ayni_task_snapshot_v1 *snapshot,
                          ayni_policy_output_v1 *out) {
    if (!out->intents || !out->intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out->intents[0] = initialized<ayni_policy_intent_v1>();
    out->intents[0].kind = AYNI_POLICY_REPORT_COMPLETION;
    out->intents[0].expected_revision = snapshot->revision;
    out->intent_count = 1;
    return AYNI_OK;
}
static ayni_status_t approval_needs(void *, ayni_bytes_v1,
                                    const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 req{
        sizeof(req), 1, text("portable.approval"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &req);
}
static ayni_status_t turn_needs(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 req{
        sizeof(req), 1, text("mock.turn"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &req);
}
// C-MEM-003/004: the decision captures its actual source before proposing a derived record.
static ayni_status_t derive(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                            ayni_policy_output_v1 *out) {
    if (snapshot->step_count) {
        return done(ctx, snapshot, out);
    }
    constexpr uint32_t identity_bytes = sizeof(uint64_t);
    constexpr uint32_t byte_bits = 8;
    if (snapshot->goal_parameters.size != identity_bytes || !snapshot->memory ||
        !out->intent_capacity) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint64_t source_id = 0;
    for (uint32_t at = 0; at < identity_bytes; ++at) {
        source_id |= uint64_t{snapshot->goal_parameters.data[at]} << (at * byte_bits);
    }
    char body[64]{};
    auto info = initialized<ayni_memory_info_v1>();
    uint32_t count = 0;
    auto status = snapshot->memory->read(snapshot->memory->context, source_id, AYNI_MEMORY_ORGANIZE,
                                         &info, body, sizeof(body), &count);
    auto proposal = initialized<ayni_memory_proposal_v1>();
    proposal.source = AYNI_MEMORY_DERIVED;
    proposal.step_key = text("derive");
    proposal.body = text("Derived fixture note.");
    if (status == AYNI_OK) {
        status = snapshot->memory->propose(snapshot->memory->context, &proposal);
    }
    if (status == AYNI_OK) {
        out->intents[0] = initialized<ayni_policy_intent_v1>();
        out->intents[0].kind = AYNI_POLICY_WRITE_MEMORY;
        out->intents[0].expected_revision = snapshot->revision;
        out->intents[0].step_key_size = proposal.step_key.size;
        std::memcpy(out->intents[0].step_key, proposal.step_key.data, proposal.step_key.size);
        out->intent_count = 1;
    }
    return status;
}
static ayni_status_t memory_needs(void *, ayni_bytes_v1,
                                  const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 req{
        sizeof(req), 1, text("memory.access"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &req);
}
ayni_status_t configure(Profile &p, Policies &policies) {
    // C-SDK-009: unconfigured input is a lifecycle refusal, not retryable BUSY.
    if (p.conversation(0)->begin(text("sum")) != AYNI_INVALID_STATE) {
        return AYNI_INVALID_STATE;
    }
    const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("portable.approval"), 0, 0, nullptr};
    auto status = ayni_register_capability(p.kernel(), &cap);
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.version = 1;
    strategy.strategy_id = text("portable.complete");
    strategy.decide = done;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    if (status == AYNI_OK) {
        status = ayni_register_strategy(p.kernel(), &strategy);
    }
    auto type = initialized<ayni_goal_type_v1>();
    type.version = 1;
    type.goal_type_id = text("portable.simple");
    type.strategy_id = strategy.strategy_id;
    type.trigger_kinds =
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_DELAY) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_LOCAL_INPUT) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    if (status == AYNI_OK) {
        status = ayni_register_goal_type(p.kernel(), &type);
    }
    type.goal_type_id = text("portable.approval");
    type.export_requirements = approval_needs;
    if (status == AYNI_OK) {
        status = ayni_register_goal_type(p.kernel(), &type);
    }
    policies.turn_input[0].kind = AYNI_VALUE_OBJECT;
    policies.turn_input[0].subtree_end = static_cast<uint32_t>(policies.turn_input.size());
    policies.turn_input[1].kind = AYNI_VALUE_I64;
    policies.turn_input[1].field_id = 1;
    policies.turn_input[1].subtree_end = static_cast<uint32_t>(policies.turn_input.size());
    policies.turn_input[1].value.integer = 5;
    policies.turn_step.name = text("turn");
    policies.turn_step.call.binding_id = text("portable.turn");
    policies.turn_step.call.input = ayni_value_v1{
        policies.turn_input.data(), static_cast<uint32_t>(policies.turn_input.size())};
    policies.turn_rule = {text("portable.turn"), 1, &policies.turn_step, 1};
    if (status == AYNI_OK) {
        status = policies.execution.configure({text("portable.execution"), 1, nullptr, 0},
                                              &policies.turn_rule, 1);
    }
    strategy = policies.execution.describe();
    if (status == AYNI_OK) {
        status = ayni_register_strategy(p.kernel(), &strategy);
    }
    type.goal_type_id = text("portable.turn");
    type.strategy_id = strategy.strategy_id;
    type.export_requirements = turn_needs;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    if (status == AYNI_OK) {
        status = ayni_register_goal_type(p.kernel(), &type);
    }
    strategy = initialized<ayni_strategy_v1>();
    strategy.version = 1;
    strategy.strategy_id = text("portable.derive");
    strategy.decide = derive;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    if (status == AYNI_OK) {
        status = ayni_register_strategy(p.kernel(), &strategy);
    }
    type.goal_type_id = text("portable.memory");
    type.strategy_id = strategy.strategy_id;
    type.validate = [](void *, ayni_bytes_v1 value) {
        return value.size == sizeof(uint64_t) ? AYNI_OK : AYNI_INVALID_ARGUMENT;
    };
    type.export_requirements = memory_needs;
    if (status == AYNI_OK) {
        status = ayni_register_goal_type(p.kernel(), &type);
    }
    return status;
}
static ayni_goal_create_v1 goal(Profile &p, const char *type, uint32_t agent = 0,
                                ayni_trigger_kind_t trigger = AYNI_TRIGGER_ONCE) {
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text(type);
    body.goal_type_version = 1;
    body.owner_instance_id = p.instance(agent);
    body.trigger = trigger;
    return body;
}
static ayni_status_t completed(Profile &p, Clock c, uint64_t id) {
    ayni_status_t queried = AYNI_OK;
    auto info = initialized<ayni_goal_info_v1>();
    const auto status = until(p, c, [&] {
        queried = ayni_get_goal(p.kernel(), id, &info);
        auto kernel = initialized<ayni_kernel_info_v1>();
        if (queried == AYNI_OK) {
            queried = ayni_get_kernel_info(p.kernel(), &kernel);
        }
        return queried != AYNI_OK ||
               ((info.lifecycle == AYNI_GOAL_SATISFIED || info.lifecycle == AYNI_GOAL_FAILED) &&
                info.commit_seq <= kernel.durable_seq);
    });
    return status != AYNI_OK                       ? status
           : queried != AYNI_OK                    ? queried
           : info.lifecycle == AYNI_GOAL_SATISFIED ? AYNI_OK
                                                   : AYNI_INVALID_STATE;
}
static ayni_status_t cancel_goal(Profile &p, Clock c, uint64_t id) {
    auto info = initialized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(p.kernel(), id, &info);
    const ayni_goal_target_v1 target{sizeof(target), 0, id};
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CANCEL, &target, info.revision);
    }
    return status;
}
// Explicit owner approval, bounded delegation, revocation and a fresh denied proposal.
static ayni_status_t authority(Profile &p, Clock c) {
    auto original = goal(p, "portable.approval");
    uint64_t id = 0;
    auto status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &original, 0, &id);
    auto info = initialized<ayni_goal_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_goal(p.kernel(), id, &info);
    }
    if (status != AYNI_OK || info.lifecycle != AYNI_GOAL_PROPOSED) {
        return AYNI_INVALID_STATE;
    }
    const ayni_goal_approve_v1 approve{sizeof(approve), 1, id, AYNI_GRANT_PERMANENT, 0, 0,
                                       original};
    status = submit(p, c, AYNI_INTENT_GOAL_APPROVE, &approve, info.revision);
    if (status == AYNI_OK) {
        status = completed(p, c, id);
    }
    if (status != AYNI_OK) {
        return status;
    }
    ayni_grant_create_v1 grant{sizeof(grant),
                               1,
                               text("portable.approval"),
                               p.instance(0),
                               0,
                               AYNI_GRANT_PERMANENT,
                               AYNI_GRANT_ALLOW_DELEGATION,
                               0,
                               {sizeof(ayni_scope_v1), 0, nullptr}};
    uint64_t parent = 0, child = 0;
    status = submit(p, c, AYNI_INTENT_GRANT_CREATE, &grant, 0, &parent);
    auto parent_info = initialized<ayni_grant_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_grant(p.kernel(), parent, &parent_info);
    }
    grant.subject_instance_id = p.instance(1);
    grant.parent_grant_id = parent;
    grant.flags = 0;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GRANT_CREATE, &grant, parent_info.revision, &child);
    }
    auto child_info = initialized<ayni_grant_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_grant(p.kernel(), child, &child_info);
    }
    if (status != AYNI_OK || child_info.parent_grant_id != parent || child_info.depth != 1) {
        return AYNI_INVALID_STATE;
    }
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, parent};
    status = submit(p, c, AYNI_INTENT_GRANT_REVOKE, &revoke, parent_info.revision);
    if (status == AYNI_OK) {
        status = ayni_get_grant(p.kernel(), child, &child_info);
    }
    if (status != AYNI_OK || child_info.effective_reason == AYNI_REASON_NONE) {
        return AYNI_INVALID_STATE;
    }
    original = goal(p, "portable.approval", 1);
    status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &original, 0, &id);
    if (status == AYNI_OK) {
        status = ayni_get_goal(p.kernel(), id, &info);
    }
    if (status != AYNI_OK || info.lifecycle != AYNI_GOAL_PROPOSED) {
        return AYNI_INVALID_STATE;
    }
    status = cancel_goal(p, c, id);
    if (status == AYNI_OK) {
        std::puts("authority approval/delegation/revocation passed");
    }
    return status;
}
// Civil time comes from the injected trusted fixture source, never the Linux wall clock.
static ayni_status_t work(Profile &p, Clock c) {
    auto body = goal(p, "portable.simple", 0, AYNI_TRIGGER_DELAY);
    body.trigger_delay_us = 100000;
    uint64_t id = 0;
    auto status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    if (status == AYNI_OK) {
        status = completed(p, c, id);
    }
    body = goal(p, "portable.simple", 0, AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    }
    auto info = initialized<ayni_goal_info_v1>();
    if (status == AYNI_OK) {
        status = until(p, c, [&] {
            return ayni_get_goal(p.kernel(), id, &info) != AYNI_OK || info.occurrences >= 2;
        });
    }
    if (status == AYNI_OK) {
        status = cancel_goal(p, c, id);
    }
    auto group = initialized<ayni_pause_group_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_pause_group(p.kernel(), 1, &group);
    }
    const ayni_pause_group_set_v1 pause{sizeof(pause), 1, 1, 0};
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_PAUSE_GROUP_SET, &pause, group.revision);
    }
    body = goal(p, "portable.simple", 0, AYNI_TRIGGER_LOCAL_INPUT);
    body.trigger_source = text("portable.input");
    body.pause_group = 1;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    }
    const ayni_event_v1 event{sizeof(event), AYNI_EVENT_LOCAL_INPUT, body.trigger_source};
    if (status == AYNI_OK) {
        status = ayni_post_event(p.kernel(), &event);
    }
    if (status == AYNI_OK) {
        status = p.tick();
    }
    if (status == AYNI_OK) {
        status = ayni_get_goal(p.kernel(), id, &info);
    }
    if (status != AYNI_OK || info.occurrences || !info.group_paused) {
        return AYNI_INVALID_STATE;
    }
    if (status == AYNI_OK) {
        status = ayni_get_pause_group(p.kernel(), 1, &group);
    }
    const ayni_pause_group_set_v1 resume{sizeof(resume), 1, 0, 0};
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_PAUSE_GROUP_SET, &resume, group.revision);
    }
    if (status == AYNI_OK) {
        status = ayni_post_event(p.kernel(), &event);
    }
    if (status == AYNI_OK) {
        status = until(p, c, [&] {
            return ayni_get_goal(p.kernel(), id, &info) != AYNI_OK || info.occurrences == 1;
        });
    }
    if (status == AYNI_OK) {
        status = cancel_goal(p, c, id);
    }
    auto calendar = initialized<ayni_calendar_rule_v1>();
    calendar.kind = AYNI_CALENDAR_ONCE;
    calendar.rule_version = 1;
    calendar.timezone_size = sizeof("UTC0") - 1;
    std::memcpy(calendar.timezone, "UTC0", calendar.timezone_size);
    calendar.first_local_s = 1893456000;
    body = goal(p, "portable.simple", 0, AYNI_TRIGGER_CALENDAR_ONCE);
    body.calendar = &calendar;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    }
    if (status == AYNI_OK) {
        status = completed(p, c, id);
    }
    auto scheduled = initialized<ayni_calendar_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_calendar(p.kernel(), id, &scheduled);
    }
    if (status != AYNI_OK || scheduled.taken != 1 ||
        scheduled.last_utc_s != calendar.first_local_s) {
        return AYNI_INVALID_STATE;
    }
    std::puts("work delay/interval/input/pause-group/calendar passed");
    return AYNI_OK;
}
static ayni_status_t execution(Profile &p, Clock c) {
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     1,
                                     text("mock.turn"),
                                     p.instance(0),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 0, nullptr}};
    auto status = submit(p, c, AYNI_INTENT_GRANT_CREATE, &grant);
    const ayni_scene_set_enabled_v1 enable{sizeof(enable), 1, text("portable.scene"), 1, 0};
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_SCENE_SET_ENABLED, &enable);
    }
    auto body = goal(p, "portable.turn");
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    }
    if (status == AYNI_OK) {
        status = completed(p, c, id);
    }
    auto result = initialized<ayni_execution_info_v1>();
    if (status == AYNI_OK) {
        status = until(p, c, [&] {
            const auto metrics = p.execution_evidence();
            auto kernel = initialized<ayni_kernel_info_v1>();
            auto resource = initialized<ayni_resource_info_v1>();
            return metrics.call &&
                   ayni_get_execution(p.kernel(), metrics.call, &result) == AYNI_OK &&
                   ayni_get_kernel_info(p.kernel(), &kernel) == AYNI_OK &&
                   ayni_get_resource(p.kernel(), text("mock.actuator"), &resource) == AYNI_OK &&
                   result.phase == AYNI_EXECUTION_STOPPED &&
                   result.requires_seq <= kernel.durable_seq && !resource.used &&
                   !resource.quarantine_id;
        });
    }
    const auto metrics = p.execution_evidence();
    auto actuator = initialized<ayni_resource_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_resource(p.kernel(), text("mock.actuator"), &actuator);
    }
    if (status != AYNI_OK || metrics.submissions != 1 || !metrics.samples || actuator.used ||
        actuator.quarantine_id || result.target != AYNI_TARGET_SATISFIED) {
        std::printf("execution status=%ld call=%llu samples=%llu submissions=%llu phase=%u "
                    "target=%u used=%u quarantine=%llu\n",
                    static_cast<long>(status), static_cast<unsigned long long>(metrics.call),
                    static_cast<unsigned long long>(metrics.samples),
                    static_cast<unsigned long long>(metrics.submissions),
                    static_cast<unsigned>(result.phase), static_cast<unsigned>(result.target),
                    static_cast<unsigned>(actuator.used),
                    static_cast<unsigned long long>(actuator.quarantine_id));
        if (metrics.call) {
            auto call = initialized<ayni_call_info_v1>();
            if (ayni_get_call(p.kernel(), metrics.call, &call) == AYNI_OK) {
                std::printf("call outcome=%u status=%ld reason=%u\n",
                            static_cast<unsigned>(call.outcome), static_cast<long>(call.status),
                            static_cast<unsigned>(call.reason));
            }
        }
        return AYNI_INVALID_STATE;
    }
    std::puts("simulated Scene/permit/output/stop passed");
    return AYNI_OK;
}
ayni_status_t start(Profile &p, Clock c) {
    auto status = p.start();
    if (status == AYNI_OK) {
        status = until(p, c, [&] { return p.running(); });
    }
    return status;
}
static ayni_status_t idle_conversations(Profile &p, Clock c) {
    return until(p, c, [&] { return p.conversation(0)->ready() && p.conversation(1)->ready(); });
}
// Real derived lineage, confirmed source identity, durable file erasure and privacy compaction.
static ayni_status_t memory(Profile &p, Clock c) {
    auto save = initialized<ayni_memory_save_v1>();
    save.name_space = AYNI_MEMORY_PRIVATE;
    save.namespace_id = p.instance(0);
    save.body = text("Independent fixture source.");
    uint64_t source = 0;
    auto status = submit(p, c, AYNI_INTENT_MEMORY_SAVE, &save, 0, &source);
    auto body = goal(p, "portable.memory");
    std::array<uint8_t, sizeof(uint64_t)> identity{};
    constexpr uint32_t byte_bits = 8;
    for (uint32_t at = 0; at < identity.size(); ++at) {
        identity[at] = static_cast<uint8_t>(source >> (at * byte_bits));
    }
    body.parameters = {identity.data(), static_cast<uint32_t>(identity.size())};
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &id);
    }
    if (status == AYNI_OK) {
        status = completed(p, c, id);
    }
    const ayni_memory_search_v1 query{
        sizeof(query), AYNI_MEMORY_PRIVATE, p.instance(0), 0, text("Derived"), {}};
    auto derived = initialized<ayni_memory_info_v1>();
    uint32_t count = 0;
    if (status == AYNI_OK) {
        status = ayni_search_memory(p.kernel(), p.session(), &query, &derived, 1, &count);
    }
    if (status != AYNI_OK || count != 1 || derived.source != AYNI_MEMORY_DERIVED ||
        derived.confirmed || derived.dependency_count != 1) {
        return AYNI_INVALID_STATE;
    }
    const ayni_memory_target_v1 confirm{sizeof(confirm), 0, derived.memory_id};
    status = submit(p, c, AYNI_INTENT_MEMORY_CONFIRM, &confirm, derived.revision);
    auto confirming = initialized<ayni_privacy_info_v1>();
    if (status == AYNI_OK) {
        status = until(p, c, [&] {
            return ayni_get_privacy_status(p.kernel(), p.session(), &confirming) == AYNI_OK &&
                   (confirming.phase == AYNI_PRIVACY_IDLE ||
                    confirming.phase == AYNI_PRIVACY_COMPLETE);
        });
        std::printf("confirmation cleanup status=%ld phase=%u\n", static_cast<long>(status),
                    static_cast<unsigned>(confirming.phase));
    }
    if (status == AYNI_OK) {
        status = p.conversation(0)->begin(text("sum"), derived.memory_id);
    }
    if (status == AYNI_OK) {
        status = p.conversation(1)->begin(text("sum"));
    }
    if (status == AYNI_OK) {
        status = idle_conversations(p, c);
    }
    if (status != AYNI_OK) {
        return status;
    }
    const auto *record = p.conversation(0)->record();
    if (!record || record->last_round != 1 || !record->round_count ||
        !record->rounds[0].answer.size) {
        return AYNI_INVALID_STATE;
    }
    std::printf("first serving input estimate=%llu\n",
                static_cast<unsigned long long>(record->last_input_estimate));
    const ayni_memory_target_v1 remove{sizeof(remove), 0, source};
    status = submit(p, c, AYNI_INTENT_MEMORY_DELETE, &remove, 1);
    auto privacy = initialized<ayni_privacy_info_v1>();
    if (status == AYNI_OK) {
        status = until(p, c, [&] {
            return ayni_get_privacy_status(p.kernel(), p.session(), &privacy) == AYNI_OK &&
                   privacy.phase == AYNI_PRIVACY_COMPLETE && p.conversation(0)->ready();
        });
    }
    if (status != AYNI_OK) {
        return status;
    }
    const auto remaining =
        ayni_get_memory(p.kernel(), p.session(), derived.memory_id, &derived, nullptr, 0, &count);
    // Completed privacy cleanup may purge the derived row entirely. A retained
    // row must be unusable; NOT_FOUND leaves its output unchanged by contract.
    if (remaining != AYNI_OK && remaining != AYNI_NOT_FOUND) {
        return remaining;
    }
    record = p.conversation(0)->record();
    if ((remaining == AYNI_OK && derived.usable) || !record || !record->round_count ||
        record->rounds[0].answer.size) {
        return AYNI_INVALID_STATE;
    }
    std::puts("memory source/derived/confirmation/context-erasure/compaction passed");
    return AYNI_OK;
}
// A management mutation may occur between application polls. The public view
// must immediately reject the old body, including before any file callback runs.
static ayni_status_t immediate_privacy_barrier(Profile &p, Clock c) {
    auto context = initialized<ayni_context_info_v1>();
    auto status = durable(p, c);
    if (status == AYNI_OK) {
        status = ayni_get_context(p.kernel(), p.session(), p.instance(1), &context);
    }
    const ayni_context_command_v1 forget{
        sizeof(forget), AYNI_CONTEXT_ERASE_ALL, 0, context.context_id, context.generation, 0, 0};
    if (status != AYNI_OK) {
        return status;
    }
    const auto accepted = p.submit(AYNI_INTENT_CONTEXT_FORGET, &forget, context.revision);
    if (accepted.status != AYNI_OK) {
        return accepted.status;
    }
    if (p.conversation(1)->record() || p.conversation(1)->begin(text("sum")) != AYNI_BUSY) {
        return AYNI_INVALID_STATE;
    }
    status = idle_conversations(p, c);
    const auto *record = p.conversation(1)->record();
    if (status != AYNI_OK || !record || record->round_count || record->last_round != 1) {
        return AYNI_INVALID_STATE;
    }
    std::puts("immediate context privacy barrier passed");
    return status;
}
// Normal context compression, not an endurance run: cross the configured trigger
// with twenty bounded 320-byte user turns and require the real summary call/record.
static ayni_status_t compression(Profile &p, Clock c) {
    constexpr uint32_t turns = 20, input_bytes = 320;
    std::array<char, input_bytes> input{};
    input.fill('x');
    for (uint32_t at = 0; at < turns; ++at) {
        const auto accepted =
            p.conversation(0)->begin({input.data(), static_cast<uint32_t>(input.size())});
        if (accepted != AYNI_OK) {
            return accepted;
        }
        const auto status = idle_conversations(p, c);
        if (status != AYNI_OK) {
            return status;
        }
    }
    const auto *record = p.conversation(0)->record();
    if (!record || !record->summary.size || !record->summary_first ||
        record->summary_last < record->summary_first || record->context_version <= 1) {
        return AYNI_INVALID_STATE;
    }
    std::puts("normal multi-turn context compression passed");
    return AYNI_OK;
}
// C-RUN-003: reaching the soft model ceiling is an owner-recoverable Core hold.
// Raising it must finish the same Task and retain exactly the original eight tool executions.
static void describe_turn(Profile &p, uint32_t index) {
    auto goal = initialized<ayni_goal_info_v1>();
    auto task = initialized<ayni_task_info_v1>();
    const auto found = ayni_get_goal(p.kernel(), p.conversation(index)->latest_goal(), &goal);
    const auto status =
        found == AYNI_OK ? ayni_get_task(p.kernel(), goal.last_task_id, &task) : found;
    std::printf("turn %u query=%ld goal_state=%u task_state=%u hold=%ld reason=%u models=%u/%u "
                "file_pending=%d\n",
                static_cast<unsigned>(index), static_cast<long>(status),
                static_cast<unsigned>(goal.lifecycle), static_cast<unsigned>(task.lifecycle),
                static_cast<long>(task.hold_status), static_cast<unsigned>(task.hold_reason),
                static_cast<unsigned>(task.model_calls_used),
                static_cast<unsigned>(task.model_call_limit),
                p.conversation(index)->file_pending());
}
static ayni_status_t resume_model_limit(Profile &p, Clock c) {
    constexpr uint32_t raised_limit = 12, expected_sums = 8;
    const auto accepted = p.conversation(0)->begin(text("many sums"));
    if (accepted != AYNI_OK) {
        return accepted;
    }
    auto task = initialized<ayni_task_info_v1>();
    auto started = c.now_us(c.context);
    uint64_t goal_id = 0;
    while (true) {
        const auto status = p.tick();
        if (status != AYNI_OK) {
            goal_id = p.conversation(0)->latest_goal();
            auto info = initialized<ayni_goal_info_v1>();
            if (ayni_get_goal(p.kernel(), goal_id, &info) != AYNI_OK ||
                ayni_get_task(p.kernel(), info.last_task_id, &task) != AYNI_OK ||
                !task.model_call_limit_reached ||
                task.hold_reason != AYNI_REASON_TASK_MODEL_CALL_LIMIT ||
                task.hold_status != status) {
                return AYNI_INVALID_STATE;
            }
            break;
        }
        if (p.conversation(0)->ready() || c.now_us(c.context) - started >= wait_us) {
            return AYNI_INVALID_STATE;
        }
        c.yield(c.context);
    }
    std::puts("model soft limit reached");
    const uint64_t task_id = task.task_id;
    const ayni_status_t expected_hold = task.hold_status;
    // The other Agent must complete its own ToolLoop/file ACK while A is held.
    auto status = p.conversation(1)->begin(text("sum"));
    if (status != AYNI_OK) {
        return status;
    }
    started = c.now_us(c.context);
    while (!p.conversation(1)->ready()) {
        status = p.tick();
        if (status != AYNI_OK && status != expected_hold) {
            return status;
        }
        if (c.now_us(c.context) - started >= wait_us) {
            describe_turn(p, 0);
            describe_turn(p, 1);
            return AYNI_EXPIRED;
        }
        c.yield(c.context);
    }
    if (ayni_get_task(p.kernel(), task_id, &task) != AYNI_OK || !task.model_call_limit_reached) {
        return AYNI_INVALID_STATE;
    }
    std::puts("B completed while A held");
    const ayni_task_raise_model_limit_v1 raise{sizeof(raise), raised_limit, task_id};
    const auto raised = p.submit(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &raise, task.revision);
    if (raised.status != AYNI_OK) {
        return raised.status;
    }
    started = c.now_us(c.context);
    while (!p.conversation(0)->ready()) {
        const auto status = p.tick();
        if (status != AYNI_OK && status != expected_hold) {
            return status;
        }
        if (c.now_us(c.context) - started >= wait_us) {
            describe_turn(p, 0);
            describe_turn(p, 1);
            return AYNI_EXPIRED;
        }
        c.yield(c.context);
    }
    if (p.conversation(0)->latest_goal() != goal_id ||
        ayni_get_task(p.kernel(), task_id, &task) != AYNI_OK ||
        task.lifecycle != AYNI_TASK_COMPLETED || task.model_calls_used != expected_sums + 1) {
        return AYNI_INVALID_STATE;
    }
    std::array<uint64_t, AYNI_CALL_CAPACITY_MAX> calls{};
    uint32_t count = 0, sums = 0;
    status = ayni_list_objects(p.kernel(), AYNI_OBJECT_CALL, calls.data(),
                               static_cast<uint32_t>(calls.size()), &count);
    for (uint32_t at = 0; status == AYNI_OK && at < count; ++at) {
        auto info = initialized<ayni_call_info_v1>();
        status = ayni_get_call(p.kernel(), calls[at], &info);
        if (info.task_id == task_id && info.result_kind == AYNI_CALL_RESULT_VALUE &&
            info.outcome == AYNI_CALL_COMPLETED) {
            ++sums;
        }
    }
    if (status != AYNI_OK || sums != expected_sums) {
        return AYNI_INVALID_STATE;
    }
    std::puts("model soft-limit owner raise resumes original Task without duplicate tools passed");
    return AYNI_OK;
}
static ayni_status_t scalar_jobs(Profile &p, Clock c) {
    std::array<uint64_t, agent_count> ids{};
    for (uint32_t at = 0; at < agent_count; ++at) {
        const auto body = goal(p, "example.managed", at);
        const auto status = submit(p, c, AYNI_INTENT_GOAL_CREATE, &body, 0, &ids[at]);
        if (status != AYNI_OK) {
            return status;
        }
    }
    for (const auto id : ids) {
        const auto status = completed(p, c, id);
        if (status != AYNI_OK) {
            return status;
        }
    }
    std::puts("typed directory/managed jobs/subscription/scalar source passed");
    return AYNI_OK;
}
static ayni_status_t sample_resources(Profile &p, Evidence &e) {
    std::array<uint64_t, runtime::portable::live_task_limit> tasks{};
    uint32_t count = 0, live = 0;
    auto status = ayni_list_objects(p.kernel(), AYNI_OBJECT_TASK, tasks.data(),
                                    static_cast<uint32_t>(tasks.size()), &count);
    for (uint32_t at = 0; status == AYNI_OK && at < count; ++at) {
        auto task = initialized<ayni_task_info_v1>();
        status = ayni_get_task(p.kernel(), tasks[at], &task);
        if (task.lifecycle == AYNI_TASK_READY || task.lifecycle == AYNI_TASK_RUNNING ||
            task.lifecycle == AYNI_TASK_WAITING || task.lifecycle == AYNI_TASK_PAUSED ||
            task.lifecycle == AYNI_TASK_CANCELLING) {
            ++live;
        }
    }
    e.peak_live_tasks = e.peak_live_tasks > live ? e.peak_live_tasks : live;
    const char *resources[agent_count]{"model.a", "model.b"};
    for (uint32_t at = 0; status == AYNI_OK && at < agent_count; ++at) {
        auto resource = initialized<ayni_resource_info_v1>();
        status = ayni_get_resource(p.kernel(), text(resources[at]), &resource);
        e.model_resource_seen[at] = e.model_resource_seen[at] || resource.used == 1;
    }
    auto logical = initialized<ayni_resource_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_resource(p.kernel(), text("model.slot"), &logical);
    }
    return status != AYNI_OK                                           ? status
           : logical.used || live > runtime::portable::live_task_limit ? AYNI_INVALID_STATE
                                                                       : AYNI_OK;
}
static ayni_status_t exact_sum(Profile &p, uint64_t call_id) {
    auto call = initialized<ayni_call_info_v1>();
    auto status = ayni_get_call(p.kernel(), call_id, &call);
    if (status != AYNI_OK || call.result_kind != AYNI_CALL_RESULT_VALUE ||
        call.outcome != AYNI_CALL_COMPLETED) {
        return AYNI_INVALID_STATE;
    }
    char encoded[64]{};
    uint32_t bytes = 0, count = 0;
    status = ayni_copy_call_result(p.kernel(), call_id, encoded, sizeof(encoded), &bytes);
    ayni_value_node_v1 node{};
    if (status == AYNI_OK) {
        status = ayni_value_read(p.value_workspace(), {reinterpret_cast<uint8_t *>(encoded), bytes},
                                 &node, 1, &count);
    }
    return status != AYNI_OK ? status
           : count == 1 && node.kind == AYNI_VALUE_I64 && node.value.integer == 131328
               ? AYNI_OK
               : AYNI_INVALID_STATE;
}
static ayni_status_t retained_call(Profile &p, uint64_t goal_id, uint64_t &sum_call) {
    auto goal_info = initialized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(p.kernel(), goal_id, &goal_info);
    std::array<uint64_t, AYNI_CALL_CAPACITY_MAX> ids{};
    uint32_t count = 0;
    if (status == AYNI_OK) {
        status = ayni_list_objects(p.kernel(), AYNI_OBJECT_CALL, ids.data(),
                                   static_cast<uint32_t>(ids.size()), &count);
    }
    uint32_t found = 0, models = 0;
    for (uint32_t at = 0; status == AYNI_OK && at < count; ++at) {
        auto info = initialized<ayni_call_info_v1>();
        status = ayni_get_call(p.kernel(), ids[at], &info);
        if (info.task_id == goal_info.last_task_id) {
            if (info.result_kind == AYNI_CALL_RESULT_VALUE) {
                sum_call = ids[at];
                ++found;
            } else if (info.outcome == AYNI_CALL_COMPLETED) {
                ++models;
            }
        }
    }
    return status != AYNI_OK           ? status
           : found == 1 && models == 2 ? exact_sum(p, sum_call)
                                       : AYNI_INVALID_STATE;
}
// A completed earlier run is preserved, never interpreted as a new empty fixture.
static ayni_status_t fresh(Profile &p) {
    std::array<uint64_t, AYNI_CALL_CAPACITY_MAX> ids{};
    for (const auto kind :
         {AYNI_OBJECT_GOAL, AYNI_OBJECT_TASK, AYNI_OBJECT_CALL, AYNI_OBJECT_GRANT}) {
        uint32_t count = 0;
        const auto status = ayni_list_objects(p.kernel(), kind, ids.data(),
                                              static_cast<uint32_t>(ids.size()), &count);
        if (status != AYNI_OK) {
            return status;
        }
        if (count) {
            return AYNI_NOT_READY;
        }
    }
    for (uint32_t at = 0; at < agent_count; ++at) {
        auto context = initialized<ayni_context_info_v1>();
        const auto status = ayni_get_context(p.kernel(), p.session(), p.instance(at), &context);
        if (status != AYNI_NOT_FOUND) {
            return status == AYNI_OK ? AYNI_NOT_READY : status;
        }
    }
    auto privacy = initialized<ayni_privacy_info_v1>();
    const auto status = ayni_get_privacy_status(p.kernel(), p.session(), &privacy);
    return status == AYNI_OK && privacy.phase != AYNI_PRIVACY_IDLE ? AYNI_NOT_READY : status;
}
static TextExpectation fingerprint(ayni_string_v1 text) {
    constexpr uint64_t basis = UINT64_C(14695981039346656037);
    constexpr uint64_t prime = UINT64_C(1099511628211);
    uint64_t result = basis;
    for (uint32_t at = 0; at < text.size; ++at) {
        result = (result ^ static_cast<unsigned char>(text.data[at])) * prime;
    }
    return {text.size, result};
}
static BodyExpectation expected_body(const strategies::ConversationRecord &record) {
    BodyExpectation out;
    out.summary = fingerprint(record.summary.view());
    out.history = fingerprint(record.history.view());
    out.generation = record.generation;
    out.revision = record.revision;
    out.context_version = record.context_version;
    out.summary_first = record.summary_first;
    out.summary_last = record.summary_last;
    for (uint32_t at = 0; at < record.round_count; ++at) {
        out.users[at] = fingerprint(record.rounds[at].user.view());
        out.answers[at] = fingerprint(record.rounds[at].answer.view());
    }
    return out;
}
static bool same_text(TextExpectation a, TextExpectation b) {
    return a.bytes == b.bytes && a.fingerprint == b.fingerprint;
}
static bool same_body(const strategies::ConversationRecord &record, const BodyExpectation &before) {
    const auto after = expected_body(record);
    if (!same_text(after.summary, before.summary) || !same_text(after.history, before.history) ||
        after.generation != before.generation || after.revision != before.revision ||
        after.context_version != before.context_version ||
        after.summary_first != before.summary_first || after.summary_last != before.summary_last) {
        return false;
    }
    for (uint32_t at = 0; at < record.round_count; ++at) {
        if (!same_text(after.users[at], before.users[at]) ||
            !same_text(after.answers[at], before.answers[at])) {
            return false;
        }
    }
    return true;
}
static bool exact(ayni_string_v1 value, const char *expected) {
    return value.size == std::strlen(expected) &&
           (!value.size || std::memcmp(value.data, expected, value.size) == 0);
}
ayni_status_t normal(Profile &p, Clock c, Evidence &e) {
    c.observe = &e;
    auto prepared = fresh(p);
    if (prepared == AYNI_OK) {
        prepared = authority(p, c);
    }
    if (prepared == AYNI_OK) {
        prepared = work(p, c);
    }
    if (prepared == AYNI_OK) {
        prepared = execution(p, c);
    }
    if (prepared != AYNI_OK) {
        std::printf("normal preparation failed %ld\n", static_cast<long>(prepared));
        return prepared;
    }
    for (uint32_t at = 0; at < agent_count; ++at) {
        e.agents[at] = p.instance(at);
        for (const auto *id : {"fixture.tools", "fixture.summary", "software.sum",
                               "software.events", "memory.access"}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             text(id),
                                             e.agents[at],
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            const auto status = submit(p, c, AYNI_INTENT_GRANT_CREATE, &grant);
            if (status != AYNI_OK) {
                std::printf("grant %s failed %ld\n", id, static_cast<long>(status));
                return status;
            }
        }
    }
    auto status = conversations(p, c);
    if (status != AYNI_OK) {
        std::printf("conversation open failed %ld\n", static_cast<long>(status));
        return status;
    }
    status = scalar_jobs(p, c);
    if (status == AYNI_OK) {
        status = memory(p, c);
    }
    if (status == AYNI_OK) {
        status = immediate_privacy_barrier(p, c);
    }
    if (status == AYNI_OK) {
        status = resume_model_limit(p, c);
    }
    if (status == AYNI_OK) {
        status = compression(p, c);
    }
    if (status != AYNI_OK) {
        std::printf("memory/managed failed %ld\n", static_cast<long>(status));
        return status;
    }
    std::array<uint64_t, agent_count> model_before{};
    for (uint32_t at = 0; at < agent_count; ++at) {
        model_before[at] = p.model_executions(at);
        e.last_round[at] = p.conversation(at)->record()->last_round + 1;
        status = p.conversation(at)->begin(text("sum"));
        if (status != AYNI_OK) {
            return status;
        }
    }
    ayni_status_t sampled = AYNI_OK;
    status = until(p, c, [&] {
        sampled = sample_resources(p, e);
        return sampled != AYNI_OK || (p.conversation(0)->ready() && p.conversation(1)->ready());
    });
    if (status == AYNI_OK) {
        status = sampled;
    }
    if (status != AYNI_OK) {
        return status;
    }
    for (uint32_t at = 0; at < agent_count; ++at) {
        const auto *record = p.conversation(at)->record();
        if (!record || record->last_round != e.last_round[at] || !record->round_count ||
            !record->rounds[record->round_count - 1].answer.size ||
            p.model_executions(at) != model_before[at] + 2 || !e.model_resource_seen[at]) {
            std::printf("record %u last %llu rounds %u executions %llu answer %u\n",
                        static_cast<unsigned>(at),
                        record ? static_cast<unsigned long long>(record->last_round) : 0,
                        static_cast<unsigned>(record ? record->round_count : 0),
                        static_cast<unsigned long long>(p.model_executions(at)),
                        static_cast<unsigned>(record ? record->rounds[0].answer.size : 0));
            return AYNI_INVALID_STATE;
        }
        e.goals[at] = p.conversation(at)->latest_goal();
        e.contexts[at] = record->id;
        e.rounds[at] = record->round_count;
        e.body[at] = expected_body(*record);
        status = retained_call(p, e.goals[at], e.calls[at]);
        if (status != AYNI_OK) {
            return status;
        }
        std::printf("agent %u goal %llu answer %.*s\n", static_cast<unsigned>(at),
                    static_cast<unsigned long long>(e.goals[at]),
                    static_cast<int>(record->rounds[record->round_count - 1].answer.size),
                    record->rounds[record->round_count - 1].answer.bytes.data());
    }
    std::printf("peak live Tasks=%u limit=%u; model.a/model.b used, model.slot unused\n",
                static_cast<unsigned>(e.peak_live_tasks),
                static_cast<unsigned>(runtime::portable::live_task_limit));
    std::puts("portable two-agent ToolLoop normal passed");
    return AYNI_OK;
}
ayni_status_t reopened(Profile &p, Clock c, const Evidence &e) {
    auto status = conversations(p, c);
    if (status != AYNI_OK) {
        return status;
    }
    for (uint32_t at = 0; at < agent_count; ++at) {
        const auto *record = p.conversation(at)->record();
        if (p.instance(at) != e.agents[at] || !record || record->id != e.contexts[at] ||
            record->last_round != e.last_round[at] || record->round_count != e.rounds[at] ||
            p.model_executions(at)) {
            return AYNI_INVALID_STATE;
        }
        const auto &last = record->rounds[record->round_count - 1];
        if (!same_body(*record, e.body[at]) || !exact(last.user.view(), "sum") ||
            !exact(last.answer.view(), at == 0 ? "A: fixture reply." : "B: fixture reply.")) {
            return AYNI_INVALID_STATE;
        }
        auto goal = initialized<ayni_goal_info_v1>();
        status = ayni_get_goal(p.kernel(), e.goals[at], &goal);
        if (status != AYNI_OK || goal.lifecycle != AYNI_GOAL_SATISFIED) {
            return AYNI_INVALID_STATE;
        }
        status = exact_sum(p, e.calls[at]);
        if (status != AYNI_OK) {
            return status;
        }
    }
    std::puts("portable orderly reopen passed");
    return AYNI_OK;
}
ayni_status_t stop(Profile &p, Clock c) {
    auto status = p.begin_shutdown();
    return status == AYNI_OK ? until(p, c, [&] { return p.stopped(); }) : status;
}
} // namespace ayni::portability
