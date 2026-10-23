// ADR-0037: public requirement export, proposals and fixed dependency consumption.
#include "work/goal/goal_records.h"
#include "work_node.h"

#include <ayni/authority.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <functional>

namespace {
using namespace ayni::test;

struct Range {
    ayni_scope_field_v1 field{};
    ayni_scope_v1 scope{};
    Range(int64_t low, int64_t high) {
        field = {sizeof(field), 1, {sizeof(field.allowed), AYNI_AUTHORITY_I64, text("count"), {}}};
        field.allowed.values.integer = {low, high};
        scope = {sizeof(scope), 1, &field};
    }
};

class AuthorityWork {
  public:
    AuthorityWork() {
        work.node().config().grant_capacity = 8;
        work.node().config().command_capacity = 16;
        work.node().on_create([this](ayni_kernel_t *kernel) {
            const auto principal = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &principal), AYNI_OK);
            for (auto id : {agent_a, agent_b}) {
                const auto agent = agent_desc(id);
                ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            }
            const auto strategy =
                strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy(), 2, 8);
            ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
            auto type = goal_type_desc("authorized", 1, nullptr, this);
            type.export_requirements = emit;
            ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            const auto zero = goal_type_desc("zero", 1);
            ASSERT_EQ(ayni_register_goal_type(kernel, &zero), AYNI_OK);
            Range range(0, 100);
            const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                     range.field.allowed};
            const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("device.move"), 1, 0, &field};
            ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
        });
    }
    void run() {
        work.node().create_and_start();
        work.node().recover();
        if (work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING) {
            work.reopen_session();
        }
    }
    void advance_work() {
        for (unsigned index = 0; index < 8; ++index) {
            work.node().step();
            work.node().sim().complete_storage();
            work.node().recover();
        }
    }
    static ayni_status_t emit(void *ctx, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
        auto &self = *static_cast<AuthorityWork *>(ctx);
        ++self.calls;
        if (self.during) {
            self.during();
        }
        if (self.custom) {
            return self.custom(sink);
        }
        for (uint32_t index = 0; index < self.count; ++index) {
            Range range(self.low[index], self.high[index]);
            const ayni_goal_requirement_v1 requirement{sizeof(requirement), 1, text("device.move"),
                                                       range.scope};
            const auto status = sink->emit(sink->ctx, &requirement);
            if (status != AYNI_OK) {
                return status;
            }
            range.field.allowed.values.integer = {-100,
                                                  1000}; // callback-local views must be copied
        }
        return AYNI_OK;
    }
    Submitted grant(int64_t minimum = 0, int64_t maximum = 100, uint64_t duration = 0,
                    uint64_t subject = 0) {
        Range range(minimum, maximum);
        const ayni_grant_create_v1 body{sizeof(body),
                                        1,
                                        text("device.move"),
                                        subject ? subject : work.instance(agent_a),
                                        0,
                                        duration ? AYNI_GRANT_CURRENT_BOOT : AYNI_GRANT_PERMANENT,
                                        0,
                                        duration,
                                        range.scope};
        auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &body);
        intent.command_id = command++;
        return work.submit(intent);
    }
    Submitted create(ayni_trigger_kind_t trigger = AYNI_TRIGGER_ONCE, uint64_t parent = 0,
                     const char *type = "authorized", uint64_t expiry = 0) {
        auto body = WorkNode::create_body(work.instance(agent_a), parent, type, 1, {}, trigger);
        body.expiry_us = expiry;
        if (trigger == AYNI_TRIGGER_LOCAL_INPUT) {
            body.trigger_source = text("button");
        }
        if (trigger == AYNI_TRIGGER_INTERVAL) {
            body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
        }
        return work.create(body);
    }
    ayni_goal_authority_info_v1 authority(uint64_t goal) {
        ayni_goal_authority_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal_authority(work.kernel(), goal, &info), AYNI_OK);
        return info;
    }
    ayni_goal_requirement_info_v1 requirement(uint64_t goal, uint32_t index) {
        ayni_goal_requirement_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal_requirement(work.kernel(), goal, index, &info), AYNI_OK);
        return info;
    }
    void revoke(uint64_t id) {
        const ayni_grant_revoke_v1 body{sizeof(body), 0, id};
        auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
        intent.command_id = command++;
        EXPECT_EQ(work.submit(intent).status, AYNI_OK);
    }
    uint64_t bind(uint64_t goal_id, uint64_t generic_id) {
        auto &model = work.kernel()->work;
        uint32_t goal_index = 0;
        uint32_t generic_index = 0;
        EXPECT_TRUE(model.goals.find(goal_id, goal_index));
        EXPECT_TRUE(model.grants.find(generic_id, generic_index));
        auto record = model.goal_authority.at(goal_index);
        EXPECT_EQ(record.count, 1U);
        const auto &generic = model.grants.at(generic_index);
        const auto &need = record.requirements[0];
        ayni::authority::NewGrant request{generic.subject_instance_id,
                                          0,
                                          generic.owner_principal_id,
                                          generic.owner_binding_revision,
                                          need.capability_id,
                                          need.capability_version,
                                          need.source_domain,
                                          nullptr,
                                          AYNI_GRANT_PERMANENT,
                                          0,
                                          0,
                                          goal_id};
        ayni::authority::GrantTable::BatchPlacement placement{};
        ayni::authority::GrantTable::Admission plan{};
        const ayni::authority::GrantContext context{model.ids.boot_epoch(),
                                                    work.node().sim().now_us(),
                                                    work.kernel()->log.durable_seq(), goal_id};
        EXPECT_EQ(
            model.grants.check_goal_create(request, need.scope, context, placement, plan).status,
            AYNI_OK);
        uint64_t grant_id = 0;
        EXPECT_TRUE(model.ids.allocate(grant_id));
        uint32_t grant_index = 0;
        EXPECT_TRUE(model.grants.insert_goal(request, need.scope, plan, grant_id, record.stamp,
                                             grant_index));
        // White-box setup for E3a's internal producer. Borrow disjoint Grant
        // bytes when replacing the same immutable ceiling; no journal claim.
        record.requirements[0].source_domain = model.grants.source_domain(grant_index);
        record.requirements[0].scope = model.grants.scope(grant_index);
        record.requirements[0].dependency = {grant_id, 1};
        EXPECT_EQ(model.goal_authority.install_validated(goal_index, record), AYNI_OK);
        return grant_id;
    }
    WorkNode work;
    uint32_t count = 1;
    std::array<int64_t, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> low{};
    std::array<int64_t, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> high{10, 80};
    uint32_t calls = 0;
    uint64_t command = 1;
    std::function<void()> during;
    std::function<ayni_status_t(const ayni_goal_requirement_sink_v1 *)> custom;
};

// ADR-0037 decisions 2/4: copied callback-local views and explicit proposal state.
TEST(GoalRequirements, missing_grant_creates_proposal_without_tasks_and_copies_scope) {
    AuthorityWork node;
    node.run();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    const auto id = created.result.object.id;
    EXPECT_EQ(node.work.goal(id).lifecycle, AYNI_GOAL_PROPOSED);
    const auto info = node.authority(id);
    EXPECT_EQ(info.phase, AYNI_GOAL_AUTHORITY_PROPOSED);
    EXPECT_EQ(info.requirement_count, 1U);
    EXPECT_EQ(info.missing_count, 1U);
    EXPECT_EQ(node.requirement(id, 0).grant_id, 0U);
    ayni_goal_requirement_field_info_v1 field{};
    field.struct_size = sizeof(field);
    ASSERT_EQ(ayni_get_goal_requirement_field(node.work.kernel(), id, 0, 0, &field), AYNI_OK);
    EXPECT_EQ(field.domain.values.integer.max_inclusive, 100);
    EXPECT_EQ(field.scope.values.integer.max_inclusive, 10);
    node.work.node().recover();
    EXPECT_EQ(node.work.goal(id).occurrences, 0U);
    EXPECT_EQ(node.work.goal(id).last_task_id, 0U);
    EXPECT_TRUE(node.work.strategy().seen.empty());
    EXPECT_EQ(node.work.change(AYNI_INTENT_GOAL_PAUSE, id).status, AYNI_INVALID_STATE);
    EXPECT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, id).status, AYNI_OK);
}

// ADR-0037 decisions 4/6: explicit zero runs; non-durable authority cannot be selected.
TEST(GoalRequirements, zero_needs_run_and_only_durable_grants_are_selected) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    const auto pending = node.create();
    ASSERT_EQ(pending.status, AYNI_OK);
    EXPECT_EQ(node.authority(pending.result.object.id).phase, AYNI_GOAL_AUTHORITY_PROPOSED);
    node.work.node().recover();
    const auto active = node.create();
    ASSERT_EQ(active.status, AYNI_OK);
    EXPECT_EQ(node.requirement(active.result.object.id, 0).grant_id, grant.result.object.id);
    EXPECT_EQ(node.authority(active.result.object.id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
    const auto zero = node.create(AYNI_TRIGGER_ONCE, 0, "zero");
    ASSERT_EQ(zero.status, AYNI_OK);
    EXPECT_EQ(node.authority(zero.result.object.id).requirement_count, 0U);
    node.advance_work();
    EXPECT_EQ(node.work.goal(active.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(node.work.goal(zero.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(node.work.goal(pending.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
}

// ADR-0037 decision 6: partial proposals retain the original single-Grant dependency.
TEST(GoalRequirements, partial_coverage_keeps_lowest_matching_grant_and_never_unions_ranges) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant(0, 20);
    ASSERT_EQ(original.status, AYNI_OK);
    ASSERT_EQ(node.grant(21, 100).status, AYNI_OK);
    ASSERT_EQ(node.grant(0, 20).status, AYNI_OK);
    node.work.node().recover();
    node.count = 2;
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    const auto id = created.result.object.id;
    EXPECT_EQ(node.requirement(id, 0).grant_id, original.result.object.id);
    EXPECT_EQ(node.requirement(id, 1).grant_id, 0U);
    EXPECT_EQ(node.authority(id).missing_count, 1U);
}

// ADR-0037 decision 2: a child only attenuates a complete original parent ceiling.
TEST(GoalRequirements, child_cannot_expand_parent_ceiling_or_use_a_proposed_parent) {
    AuthorityWork node;
    node.run();
    const auto proposed = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(proposed.status, AYNI_OK);
    EXPECT_EQ(node.create(AYNI_TRIGGER_NONE, proposed.result.object.id).status, AYNI_INVALID_STATE);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto parent = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(parent.status, AYNI_OK);
    node.high[0] = 11;
    const auto rejected = node.create(AYNI_TRIGGER_NONE, parent.result.object.id);
    EXPECT_EQ(rejected.status, AYNI_DENIED);
    EXPECT_EQ(rejected.error.reason, AYNI_REASON_GOAL_AUTHORITY_PARENT);
    node.high[0] = 5;
    EXPECT_EQ(node.create(AYNI_TRIGGER_NONE, parent.result.object.id).status, AYNI_OK);
    const auto zero = node.create(AYNI_TRIGGER_NONE, 0, "zero");
    ASSERT_EQ(zero.status, AYNI_OK);
    const auto expanded = node.create(AYNI_TRIGGER_NONE, zero.result.object.id);
    EXPECT_EQ(expanded.status, AYNI_DENIED);
    EXPECT_EQ(expanded.error.reason, AYNI_REASON_GOAL_AUTHORITY_PARENT);
}

// C-AUTH-028: exporting another Goal cannot overwrite a retained ceiling,
// including when a callback fails after a successful emission.
TEST(GoalRequirements, failed_export_keeps_other_goals_original_scope) {
    AuthorityWork node;
    node.run();
    const auto original = node.create();
    ASSERT_EQ(original.status, AYNI_OK);
    node.custom = [](const auto *sink) {
        Range range(0, 90);
        const ayni_goal_requirement_v1 input{sizeof(input), 1, text("device.move"), range.scope};
        EXPECT_EQ(sink->emit(sink->ctx, &input), AYNI_OK);
        return AYNI_INVALID_ARGUMENT;
    };
    EXPECT_EQ(node.create().status, AYNI_INVALID_ARGUMENT);
    ayni_goal_requirement_field_info_v1 field{};
    field.struct_size = sizeof(field);
    ASSERT_EQ(ayni_get_goal_requirement_field(node.work.kernel(), original.result.object.id, 0, 0,
                                              &field),
              AYNI_OK);
    EXPECT_EQ(field.scope.values.integer.max_inclusive, 10);
    EXPECT_EQ(node.authority(original.result.object.id).phase, AYNI_GOAL_AUTHORITY_PROPOSED);
}

// C-AUTH-029: a child's independent Grant does not bypass the parent's gate.
TEST(GoalRequirements, a_live_child_grant_cannot_bypass_a_revoked_parent_dependency) {
    AuthorityWork node;
    node.run();
    const auto narrow = node.grant(0, 5);
    const auto wide = node.grant(0, 100);
    ASSERT_EQ(narrow.status, AYNI_OK);
    ASSERT_EQ(wide.status, AYNI_OK);
    node.work.node().recover();
    node.high[0] = 100;
    const auto parent = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(parent.status, AYNI_OK);
    node.high[0] = 5;
    const auto child = node.create(AYNI_TRIGGER_ONCE, parent.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    EXPECT_EQ(node.requirement(child.result.object.id, 0).grant_id, narrow.result.object.id);
    node.revoke(wide.result.object.id);
    node.advance_work();
    EXPECT_EQ(node.work.goal(child.result.object.id).last_task_id, 0U);
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// ADR-0037 decision 13: restoring authority availability does not clear an
// authorization pause or change the user's optimistic concurrency revision.
TEST(GoalRequirements, authorization_pause_is_sticky_and_preserves_user_revision) {
    AuthorityWork node;
    node.run();
    const auto created = node.create(AYNI_TRIGGER_NONE, 0, "zero");
    ASSERT_EQ(created.status, AYNI_OK);
    const auto id = created.result.object.id;
    const auto before = node.work.goal(id);
    // White-box setup: E2b has no reauthorization entry point yet. Invoke the
    // real pause transition while the saved explicit-zero ceiling stays valid.
    uint32_t index = 0;
    ASSERT_TRUE(node.work.kernel()->work.goals.find(id, index));
    node.work.kernel()->work.pause_goal_authority(index);
    const auto info = node.authority(id);
    EXPECT_EQ(info.phase, AYNI_GOAL_AUTHORITY_PAUSED);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GOAL_AUTHORITY_PAUSED);
    EXPECT_EQ(node.work.goal(id).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(node.work.goal(id).revision, before.revision);
}

// ADR-0037: all exporter failures leave live work, IDs and output unchanged.
TEST(GoalRequirements, exporter_failure_is_sticky_and_refusal_allocates_nothing) {
    AuthorityWork node;
    node.run();
    node.custom = [](const auto *sink) {
        EXPECT_EQ(sink->emit(sink->ctx, nullptr), AYNI_INVALID_ARGUMENT);
        Range range(0, 1);
        const ayni_goal_requirement_v1 input{sizeof(input), 1, text("device.move"), range.scope};
        EXPECT_EQ(sink->emit(sink->ctx, &input), AYNI_INVALID_ARGUMENT);
        return AYNI_OK; // cannot swallow the sink's first failure
    };
    const auto refused = node.create();
    EXPECT_EQ(refused.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_GOAL_REQUIREMENT_INPUT);
    node.custom = {};
    node.count = 0;
    const auto next = node.create();
    ASSERT_EQ(next.status, AYNI_OK);
    // White-box observation only: no hidden work or consumed identity on refusal.
    EXPECT_EQ(node.work.kernel()->work.goals.size(), 1U);
    EXPECT_EQ(node.work.kernel()->work.goal_authority.at(0).count, 0U);
    EXPECT_EQ(node.calls, 2U);
}

// ADR-0037: callback reentry is refused; exact catalog/Scope/capacity failures remain classified.
TEST(GoalRequirements, invalid_exports_and_reentry_preserve_the_original_status) {
    AuthorityWork node;
    node.work.node().config().goal_requirement_capacity = 1;
    node.run();
    node.during = [&] { EXPECT_EQ(node.create().status, AYNI_INVALID_STATE); };
    node.count = 2;
    auto refused = node.create();
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_GOAL_REQUIREMENT_CAPACITY);
    node.during = {};
    node.count = 1;
    for (const auto status :
         {AYNI_NOT_FOUND, AYNI_UNSUPPORTED_VERSION, AYNI_INVALID_ARGUMENT, AYNI_STORAGE_FAILED}) {
        node.custom = [status](const auto *sink) {
            if (status == AYNI_STORAGE_FAILED) {
                return status;
            }
            Range range(0, status == AYNI_INVALID_ARGUMENT ? 101 : 1);
            const ayni_goal_requirement_v1 input{
                sizeof(input), status == AYNI_UNSUPPORTED_VERSION ? 2U : 1U,
                text(status == AYNI_NOT_FOUND ? "missing" : "device.move"), range.scope};
            return sink->emit(sink->ctx, &input);
        };
        EXPECT_EQ(node.create().status, status);
    }
    EXPECT_EQ(node.work.kernel()->work.goals.size(), 0U); // White-box absence after refusals
}

// ADR-0037: every query failure preserves the complete caller-owned output.
TEST(GoalRequirements, query_failures_publish_no_partial_fields) {
    AuthorityWork node;
    node.run();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    ayni_goal_requirement_field_info_v1 out;
    std::memset(&out, 0x5a, sizeof(out));
    out.struct_size = sizeof(out);
    const auto before = out;
    for (const auto id : {created.result.object.id, uint64_t{0}}) {
        EXPECT_EQ(ayni_get_goal_requirement_field(node.work.kernel(), id, 0, 1, &out),
                  AYNI_NOT_FOUND);
        EXPECT_EQ(std::memcmp(&out, &before, sizeof(out)), 0);
    }
    EXPECT_EQ(
        ayni_get_goal_requirement_field(node.work.kernel(), created.result.object.id, 1, 0, &out),
        AYNI_NOT_FOUND);
    EXPECT_EQ(std::memcmp(&out, &before, sizeof(out)), 0);
}

// ADR-0037 minimum gate: an accepted revoke blocks before durable cleanup or derivation.
TEST(GoalRequirements, revoke_blocks_derivation_and_user_resume_does_not_reauthorize) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(grant.result.object.id);
    node.advance_work();
    const auto id = goal.result.object.id;
    EXPECT_EQ(node.work.goal(id).last_task_id, 0U);
    EXPECT_EQ(node.authority(id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    EXPECT_EQ(node.work.change(AYNI_INTENT_GOAL_PAUSE, id).status, AYNI_OK);
    EXPECT_EQ(node.work.change(AYNI_INTENT_GOAL_RESUME, id).status, AYNI_OK);
    EXPECT_EQ(node.work.goal(id).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(node.requirement(id, 0).grant_id, grant.result.object.id);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    EXPECT_EQ(node.work.goal(id).last_task_id, 0U);
}

// ADR-0037 minimum gate: READY work cannot call its strategy after dependency revocation.
TEST(GoalRequirements, revoke_before_decision_preserves_ready_task_progress) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().step();
    const auto task = node.work.goal(created.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    node.revoke(grant.result.object.id);
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    EXPECT_EQ(node.work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(node.work.task(task).step_count, 0U);
}

// ADR-0037 minimum gate: callback elapsed time cannot commit steps/checkpoints after expiry.
TEST(GoalRequirements, deadline_crossed_inside_strategy_discards_all_returned_progress) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant(0, 100, 10000000);
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().step();
    const auto task = node.work.goal(created.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    node.work.strategy().answer = [&](const auto &snapshot, auto &out) {
        node.work.node().sim().advance_us(10000000);
        out.intents[0] = progress_intent("late", snapshot.revision);
        out.intent_count = 1;
        out.checkpoint[0] = 42;
        out.checkpoint_size = 1;
        return AYNI_OK;
    };
    node.work.node().step();
    EXPECT_EQ(node.work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(node.work.task(task).step_count, 0U);
    // White-box: the ABI intentionally does not expose private checkpoint bytes.
    uint32_t index = 0;
    ASSERT_TRUE(node.work.kernel()->work.tasks.find(task, index));
    EXPECT_EQ(node.work.kernel()->work.tasks.checkpoint(index).size, 0U);
    EXPECT_EQ(node.authority(created.result.object.id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
}

// ADR-0037: proposals persist without occurrences; original expiry still applies.
TEST(GoalRequirements, proposed_work_round_trips_and_expires_without_running) {
    AuthorityWork node;
    node.run();
    const auto created = node.create(AYNI_TRIGGER_INTERVAL, 0, "authorized", 1000000);
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    node.work.reopen_session();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.authority(created.result.object.id).phase, AYNI_GOAL_AUTHORITY_PROPOSED);
    EXPECT_EQ(node.work.goal(created.result.object.id).occurrences, 0U);
    EXPECT_EQ(node.work.goal(created.result.object.id).last_task_id, 0U);
    EXPECT_EQ(node.work.goal(created.result.object.id).hold_reason, AYNI_REASON_NONE);
    node.work.node().sim().advance_us(1000001);
    node.advance_work();
    EXPECT_EQ(node.work.goal(created.result.object.id).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_TRUE(node.work.strategy().seen.empty());
}
// ADR-0037 decision 12: selection uses the clock after the exporter returns.
TEST(GoalRequirements, expiry_during_export_cannot_select_stale_authority) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant(0, 100, 10000000).status, AYNI_OK);
    node.work.node().recover();
    node.during = [&] { node.work.node().sim().advance_us(10000000); };
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(node.authority(created.result.object.id).phase, AYNI_GOAL_AUTHORITY_PROPOSED);
    EXPECT_EQ(node.requirement(created.result.object.id, 0).grant_id, 0U);
}

// C-AUTH-002/029: another instance's matching range grants no authority here.
TEST(GoalRequirements, a_matching_grant_for_another_subject_is_not_selected) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant(0, 100, 0, node.work.instance(agent_b)).status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(node.authority(created.result.object.id).missing_count, 1U);
    EXPECT_EQ(node.requirement(created.result.object.id, 0).grant_id, 0U);
}

// ADR-0037 decision 16: after internal setup, public consumption/selection
// and management queries must keep exact Goal ownership and terminal denial.
TEST(GoalGrants, a_bound_grant_cannot_authorize_other_or_child_goals_and_ends_with_its_goal) {
    AuthorityWork node;
    node.run();
    const auto generic = node.grant();
    ASSERT_EQ(generic.status, AYNI_OK);
    node.work.node().recover();
    const auto owner = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(owner.status, AYNI_OK);
    node.work.node().recover();
    const auto bound_id = node.bind(owner.result.object.id, generic.result.object.id);
    node.revoke(generic.result.object.id);
    EXPECT_EQ(node.authority(owner.result.object.id).effective_reason, AYNI_REASON_NONE);
    EXPECT_EQ(node.requirement(owner.result.object.id, 0).grant_id, bound_id);
    for (const auto parent : {uint64_t{0}, owner.result.object.id}) {
        const auto other = node.create(AYNI_TRIGGER_NONE, parent);
        ASSERT_EQ(other.status, AYNI_OK);
        EXPECT_EQ(node.authority(other.result.object.id).phase, AYNI_GOAL_AUTHORITY_PROPOSED);
        EXPECT_EQ(node.requirement(other.result.object.id, 0).grant_id, 0U);
    }
    ayni_grant_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound_id, &info), AYNI_OK);
    EXPECT_EQ(info.bound_goal_id, owner.result.object.id);
    EXPECT_EQ(info.allow_delegation, 0U);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_NONE);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, owner.result.object.id).status, AYNI_OK);
    ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound_id, &info), AYNI_OK);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GRANT_GOAL);
    EXPECT_EQ(info.lifecycle, AYNI_GRANT_ACTIVE); // persistent cleanup is a later transition
}

// ADR-0037 decision 16: reusing a terminal Goal slot never transfers its bound authority.
TEST(GoalGrants, a_reclaimed_goal_identity_cannot_authorize_the_new_slot_owner) {
    AuthorityWork node;
    node.work.node().config().goal_capacity = 1;
    node.run();
    const auto generic = node.grant();
    ASSERT_EQ(generic.status, AYNI_OK);
    node.work.node().recover();
    const auto owner = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(owner.status, AYNI_OK);
    node.work.node().recover();
    const auto bound = node.bind(owner.result.object.id, generic.result.object.id);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, owner.result.object.id).status, AYNI_OK);
    node.work.node().recover();
    const auto next = node.create(AYNI_TRIGGER_NONE, 0, "zero");
    ASSERT_EQ(next.status, AYNI_OK);
    EXPECT_NE(next.result.object.id, owner.result.object.id);
    ayni_grant_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound, &info), AYNI_OK);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GRANT_GOAL);
}

// C-AUTH-031: exact Goal identity does not substitute for its work-owned subject.
TEST(GoalGrants, a_bound_fact_with_another_subject_is_not_usable) {
    AuthorityWork node;
    node.run();
    const auto owner_grant = node.grant();
    ASSERT_EQ(owner_grant.status, AYNI_OK);
    ASSERT_EQ(node.grant(0, 100, 0, node.work.instance(agent_b)).status, AYNI_OK);
    node.work.node().recover();
    const auto goal =
        node.work.create(WorkNode::create_body(node.work.instance(agent_b), 0, "authorized"));
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().recover();
    // White-box fault: bind the other subject's fact to this Goal. Grant owns
    // shape checks; WorkModel must complete the separate subject invariant.
    const auto bound = node.bind(goal.result.object.id, owner_grant.result.object.id);
    ayni_grant_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound, &info), AYNI_OK);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GRANT_GOAL);
    EXPECT_EQ(node.authority(goal.result.object.id).effective_reason, AYNI_REASON_GRANT_GOAL);
}

} // namespace
