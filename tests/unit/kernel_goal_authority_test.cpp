// ADR-0037: public requirement export, proposals and fixed dependency consumption.
// White-box table/count/stamp assertions below supplement public observations to
// prove refusal leaves no hidden allocation or partial transition behind.
#include "foundation/little_endian.h"
#include "goal_approval_request.h"
#include "goal_reauthorization_request.h"
#include "kernel/runtime/storage/storage_budget.h"
#include "kernel/service/decision/decision.h"
#include "work/goal/goal_records.h"
#include "work_node.h"

#include <ayni/authority.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

using ayni::kernel::KernelTestProbe;

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
                if (id == agent_b && !agent_b_available) {
                    continue;
                }
                const auto agent = agent_desc(id);
                ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            }
            const auto strategy =
                strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy(), 2, 8);
            ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
            auto type = goal_type_desc("authorized", 1, nullptr, this);
            type.export_requirements = emit;
            type.trigger_kinds = trigger_kinds;
            if (allow_parameters) {
                type.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
            }
            ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            const auto zero = goal_type_desc("zero", 1);
            ASSERT_EQ(ayni_register_goal_type(kernel, &zero), AYNI_OK);
            Range range(0, domain_max);
            const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                     range.field.allowed};
            const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("device.move"), 1, 0, &field};
            if (capability_available) {
                ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
            }
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
        auto &model = KernelTestProbe::work(work.kernel());
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
        const ayni::authority::GrantContext context{
            model.ids.boot_epoch(), work.node().sim().now_us(),
            KernelTestProbe::log(work.kernel()).durable_seq(), goal_id};
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
    int64_t domain_max = 100;
    bool capability_available = true;
    bool agent_b_available = true;
    bool allow_parameters = false;
    uint32_t count = 1;
    uint32_t trigger_kinds = any_trigger;
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
    ASSERT_TRUE(KernelTestProbe::work(node.work.kernel()).goals.find(id, index));
    KernelTestProbe::work(node.work.kernel()).pause_goal_authority(index);
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
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).goals.size(), 1U);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).goal_authority.at(0).count, 0U);
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
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).goals.size(),
              0U); // White-box absence after refusals
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
    ayni_task_authority_reference_v1 selected{};
    selected.struct_size = sizeof(selected);
    ASSERT_EQ(ayni_get_task_authority_reference(node.work.kernel(), task, 0, &selected), AYNI_OK);
    const auto alternative = node.grant();
    ASSERT_EQ(alternative.status, AYNI_OK);
    node.work.node().recover();
    ASSERT_EQ(node.work.task(task).lifecycle, AYNI_TASK_READY);
    node.revoke(grant.result.object.id);
    // ADR-0038: public queries must reflect the immediate Gate before cleanup,
    // while retaining the selected dependency even with another usable Grant.
    EXPECT_EQ(node.work.task(task).lifecycle, AYNI_TASK_PAUSED);
    ayni_task_authority_info_v1 admission{};
    admission.struct_size = sizeof(admission);
    ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), task, &admission), AYNI_OK);
    EXPECT_EQ(admission.phase, AYNI_TASK_AUTHORITY_AUTHORIZED);
    EXPECT_NE(admission.effective_reason, AYNI_REASON_NONE);
    ayni_task_authority_reference_v1 retained{};
    retained.struct_size = sizeof(retained);
    ASSERT_EQ(ayni_get_task_authority_reference(node.work.kernel(), task, 0, &retained), AYNI_OK);
    EXPECT_EQ(retained.grant_id, selected.grant_id);
    EXPECT_EQ(retained.grant_revision, selected.grant_revision);
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    EXPECT_EQ(node.work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(node.work.task(task).step_count, 0U);
}

// ADR-0038: a persisted deny is quiescent, while unrelated admitted work runs.
TEST(WorkAuthorityCleanup, revoked_ready_work_stops_writing_and_other_goals_run) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto denied = node.create(AYNI_TRIGGER_INTERVAL);
    const auto usable = node.create(AYNI_TRIGGER_INTERVAL, 0, "zero");
    ASSERT_EQ(denied.status, AYNI_OK);
    ASSERT_EQ(usable.status, AYNI_OK);
    node.work.node().step(2); // Both Tasks are READY before the revoke.
    const auto task = node.work.goal(denied.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    node.revoke(grant.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen[0].snapshot.goal_id, usable.result.object.id);
    ayni_task_authority_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), task, &info), AYNI_OK);
    EXPECT_EQ(info.phase, AYNI_TASK_AUTHORITY_PAUSED);
    const auto before = KernelTestProbe::work(node.work.kernel()).transitions.latest().commit_seq;
    for (unsigned round = 0; round < 8U; ++round) {
        const auto report = node.work.node().step(1);
        EXPECT_EQ(report.items_done, 0U);
        EXPECT_EQ(report.more_ready, 0U);
    }
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).transitions.latest().commit_seq, before);
}

// C-AUTH-026/030 and ADR-0038: storage backpressure delays durable cleanup,
// while the accepted Gate immediately denies Task consumption and queries.
TEST(WorkAuthorityCleanup, a_storage_shortage_cannot_open_a_revoked_ready_task) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step(1);
    node.work.node().recover();
    const auto task = node.work.goal(goal.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    node.work.node().sim().set_storage_capacity(0);
    node.revoke(grant.result.object.id);
    for (unsigned round = 0; round < 8U; ++round) {
        node.work.node().step(1);
        EXPECT_EQ(node.work.task(task).lifecycle, AYNI_TASK_PAUSED);
        EXPECT_TRUE(node.work.strategy().seen.empty());
    }
    node.work.node().sim().set_storage_capacity(UINT64_MAX);
    node.advance_work();
    EXPECT_EQ(node.authority(goal.result.object.id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// ADR-0038: authorization adds a separate deny, preserving user/policy state,
// steps and private checkpoint through the real journal and boot reader.
TEST(WorkAuthorityCleanup, policy_and_user_holds_survive_authority_pause_and_restart) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto policy = node.create(AYNI_TRIGGER_INTERVAL);
    const auto user = node.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(policy.status, AYNI_OK);
    ASSERT_EQ(user.status, AYNI_OK);
    node.work.node().step(2);
    node.work.strategy().answer = [](const auto &snapshot, auto &out) {
        out.intents[0] = progress_intent("saved", snapshot.revision);
        out.intent_count = 1;
        out.checkpoint[0] = 41;
        out.checkpoint_size = 1;
        return AYNI_OK;
    };
    node.work.node().step();
    node.work.strategy().answer = cannot_decide;
    node.work.node().step();
    const auto task = node.work.goal(policy.result.object.id).last_task_id;
    const auto before = node.work.task(task);
    ASSERT_EQ(before.step_count, 1U);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_PAUSE, user.result.object.id).status, AYNI_OK);
    node.revoke(node.requirement(policy.result.object.id, 0).grant_id);
    node.advance_work();
    EXPECT_EQ(node.work.task(task).revision, before.revision);
    EXPECT_EQ(node.work.task(task).hold_reason, before.hold_reason);
    EXPECT_EQ(node.authority(user.result.object.id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    auto &model = KernelTestProbe::work(node.work.kernel());
    uint32_t goal_index = 0;
    ASSERT_TRUE(model.goals.find(user.result.object.id, goal_index));
    EXPECT_EQ(model.goals.at(goal_index).lifecycle, AYNI_GOAL_PAUSED);
    const auto image = node.work.node().sim().durable_storage();
    AuthorityWork restored;
    restored.work.node().sim().durable_storage() = image;
    restored.run();
    ASSERT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    // White-box: checkpoint bytes are deliberately private to the strategy.
    uint32_t task_index = 0;
    ASSERT_TRUE(KernelTestProbe::work(restored.work.kernel()).tasks.find(task, task_index));
    const auto &tasks = KernelTestProbe::work(restored.work.kernel()).tasks;
    EXPECT_EQ(tasks.at(task_index).hold_reason, before.hold_reason);
    EXPECT_EQ(tasks.at(task_index).step_count, 1U);
    ASSERT_EQ(tasks.checkpoint(task_index).size, 1U);
    EXPECT_EQ(tasks.checkpoint(task_index).data[0], 41U);
    EXPECT_EQ(restored.authority(policy.result.object.id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
}

// ADR-0038: fixed through/monotone after and a next-pass request are durable;
// later Goals cannot extend the current scan or be lost at a process restart.
TEST(WorkAuthorityCleanup, interrupted_scan_keeps_its_frontier_and_later_request) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    const auto unused = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    ASSERT_EQ(unused.status, AYNI_OK);
    node.work.node().recover();
    std::array<uint64_t, 3> goals{};
    for (auto &id : goals) {
        const auto created = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(created.status, AYNI_OK);
        id = created.result.object.id;
    }
    node.work.node().recover();
    node.revoke(grant.result.object.id);
    node.work.node().recover();
    auto &scan = KernelTestProbe::work(node.work.kernel()).authority_cleanup;
    // D's Grant-descendant cleanup has its own budget before the work scan.
    for (unsigned round = 0; round < 16U && scan.record().after_goal_id == 0U; ++round) {
        node.work.node().step(1);
        node.work.node().sim().complete_storage();
        node.work.node().recover();
    }
    ASSERT_TRUE(scan.ready());
    const auto frontier = scan.record();
    EXPECT_EQ(frontier.after_goal_id, goals[0]);
    const auto later = node.create(AYNI_TRIGGER_NONE, 0, "zero");
    ASSERT_EQ(later.status, AYNI_OK);
    node.revoke(unused.result.object.id);
    EXPECT_EQ(scan.record().through_goal_id, frontier.through_goal_id);
    EXPECT_EQ(scan.record().after_goal_id, frontier.after_goal_id);
    EXPECT_EQ(scan.record().requested_through, later.result.object.id);
    node.work.node().recover(); // Flush committed batches without advancing the scan.
    const auto persisted = scan.record();
    node.work.node().power_cut_and_start(ayni::hosts::sim::CutAppend::lost);
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const auto recovered = KernelTestProbe::work(node.work.kernel()).authority_cleanup.record();
    EXPECT_EQ(recovered.after_goal_id, persisted.after_goal_id);
    EXPECT_EQ(recovered.through_goal_id, persisted.through_goal_id);
    EXPECT_EQ(recovered.requested_through, persisted.requested_through);
    node.advance_work();
    node.advance_work();
    for (const auto id : goals) {
        EXPECT_EQ(node.authority(id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    }
    EXPECT_FALSE(KernelTestProbe::work(node.work.kernel()).authority_cleanup.ready());
    EXPECT_EQ(node.authority(later.result.object.id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
}

// C-AUTH-020/C-STORE-014: an unrelated old-boot expiry may request a scan,
// but a temporary missing/changed catalog cannot persist an authority denial.
TEST(WorkAuthorityCleanup, a_recovery_scan_keeps_catalog_holds_volatile) {
    for (const bool missing : {false, true}) {
        SCOPED_TRACE(missing);
        AuthorityWork source;
        source.run();
        ASSERT_EQ(source.grant().status, AYNI_OK);
        ASSERT_EQ(source.grant(0, 100, 10000000).status, AYNI_OK);
        source.work.node().recover();
        const auto created = source.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(created.status, AYNI_OK);
        source.work.node().recover();
        AuthorityWork incompatible;
        incompatible.capability_available = !missing;
        incompatible.domain_max = 200;
        incompatible.work.node().sim().durable_storage() =
            source.work.node().sim().durable_storage();
        incompatible.run();
        ASSERT_EQ(incompatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        ASSERT_TRUE(KernelTestProbe::work(incompatible.work.kernel()).authority_cleanup.ready());
        incompatible.advance_work();
        EXPECT_EQ(incompatible.authority(created.result.object.id).phase,
                  AYNI_GOAL_AUTHORITY_AUTHORIZED);
        AuthorityWork compatible;
        compatible.work.node().sim().durable_storage() =
            incompatible.work.node().sim().durable_storage();
        compatible.run();
        ASSERT_EQ(compatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(compatible.authority(created.result.object.id).phase,
                  AYNI_GOAL_AUTHORITY_AUTHORIZED);
        EXPECT_EQ(compatible.authority(created.result.object.id).effective_reason,
                  AYNI_REASON_NONE);
    }
}

// ADR-0038: an ended Goal's dedicated root gets its own terminal reason and
// becomes reclaimable, including after its Goal storage slot has been reused.
TEST(WorkAuthorityCleanup, dedicated_grant_ends_and_reclaims_after_goal_slot_reuse) {
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        AuthorityWork node;
        node.work.node().config().grant_capacity = 1;
        node.work.node().config().goal_capacity = 1;
        node.run();
        const auto goal = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(goal.status, AYNI_OK);
        node.work.node().recover();
        ApprovalRequest request(node.work.kernel(), goal.result.object.id);
        auto intent =
            node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
        intent.command_id = 100;
        ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
        node.work.node().recover();
        const auto bound = node.requirement(goal.result.object.id, 0).grant_id;
        ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, goal.result.object.id).status, AYNI_OK);
        node.work.node().recover();
        uint64_t replacement = 0;
        if (reuse) {
            const auto created = node.create(AYNI_TRIGGER_NONE, 0, "zero");
            ASSERT_EQ(created.status, AYNI_OK);
            replacement = created.result.object.id;
        }
        node.advance_work();
        ayni_grant_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound, &info), AYNI_OK);
        EXPECT_EQ(info.lifecycle, AYNI_GRANT_GOAL_ENDED);
        EXPECT_EQ(info.bound_goal_id, goal.result.object.id);
        if (reuse) {
            EXPECT_EQ(node.authority(replacement).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
        }
        ASSERT_EQ(node.grant().status, AYNI_OK);
        EXPECT_EQ(ayni_get_grant(node.work.kernel(), bound, &info), AYNI_NOT_FOUND);
    }
}

// ADR-0038 decision 3: changing a parent's generation immediately invalidates
// its child's saved admission, independently of an eventual cleanup scan.
TEST(GoalRequirements, parent_generation_change_immediately_denies_child_consumption) {
    AuthorityWork node;
    node.count = 0;
    node.run();
    const auto parent = node.create();
    ASSERT_EQ(parent.status, AYNI_OK);
    const auto child = node.create(AYNI_TRIGGER_ONCE, parent.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    auto &model = KernelTestProbe::work(node.work.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(model.goals.find(parent.result.object.id, index));
    auto record = model.goal_authority.at(index);
    // White-box generation change: F4 will own the complete public transaction.
    record.stamp = model.transitions.next();
    record.binding.established_seq = record.stamp.commit_seq;
    ASSERT_TRUE(ayni::work::valid_goal_authority_record(record));
    ASSERT_EQ(model.goal_authority.install_validated(index, record), AYNI_OK);
    EXPECT_EQ(node.authority(child.result.object.id).effective_reason,
              AYNI_REASON_GOAL_AUTHORITY_PARENT);
    node.work.node().step();
    EXPECT_EQ(node.work.goal(child.result.object.id).last_task_id, 0U);
}

// C-STORE-020: the selected revision belongs to Task admission as well as Goal
// admission. White-box alteration must be visible through consumption/query.
TEST(TaskAuthorityApi, a_task_cannot_substitute_its_selected_grant_revision) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step(1);
    const auto id = node.work.goal(goal.result.object.id).last_task_id;
    auto &model = KernelTestProbe::work(node.work.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(model.tasks.find(id, index));
    auto record = model.task_authority.at(model.tasks.at(index).slot);
    ++record.references[0].revision;
    ASSERT_EQ(model.task_authority.install_validated(model.tasks.at(index).slot, record), AYNI_OK);
    ayni_task_authority_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), id, &info), AYNI_OK);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GOAL_AUTHORITY_MISSING);
    node.work.node().step();
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// C-STORE-020: two disjoint requirements inherit their own selections in order.
TEST(TaskAuthorityApi, multiple_requirements_keep_distinct_selected_grants) {
    AuthorityWork node;
    node.count = 2;
    node.low[1] = 50;
    node.run();
    const auto first = node.grant(0, 20);
    ASSERT_EQ(first.status, AYNI_OK);
    node.work.node().recover();
    const auto second = node.grant(40, 100);
    ASSERT_EQ(second.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step(1);
    const auto task = node.work.goal(goal.result.object.id).last_task_id;
    uint32_t at = 0;
    for (const auto id : {first.result.object.id, second.result.object.id}) {
        ayni_task_authority_reference_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_task_authority_reference(node.work.kernel(), task, at++, &info),
                  AYNI_OK);
        EXPECT_EQ(info.grant_id, id);
        EXPECT_EQ(info.grant_revision, 1U);
    }
}

// C-AUTH-023: Task references independently prevent reclamation until terminal
// state is durable. White-box removal isolates them from the Goal's same refs.
TEST(TaskAuthorityApi, live_task_references_and_unconfirmed_terminal_state_prevent_reclaim) {
    AuthorityWork node;
    node.work.node().config().grant_capacity = 1;
    node.run();
    constexpr uint64_t duration_us = 1000000;
    const auto grant = node.grant(0, 100, duration_us);
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step(1);
    ASSERT_NE(node.work.goal(goal.result.object.id).last_task_id, 0U);
    node.work.node().sim().advance_us(duration_us);
    node.work.node().step(1);
    node.work.node().recover();
    auto &model = KernelTestProbe::work(node.work.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(model.goals.find(goal.result.object.id, index));
    auto saved = model.goal_authority.at(index);
    const auto &need = saved.requirements[0];
    const std::vector<uint8_t> domain(need.source_domain.data,
                                      need.source_domain.data + need.source_domain.size);
    const std::vector<uint8_t> scope(need.scope.data, need.scope.data + need.scope.size);
    saved.requirements[0].source_domain = {domain.data(), static_cast<uint32_t>(domain.size())};
    saved.requirements[0].scope = {scope.data(), static_cast<uint32_t>(scope.size())};
    model.goal_authority.clear(index);
    EXPECT_TRUE(
        model.grant_reclaim_exclusions(KernelTestProbe::log(node.work.kernel()).durable_seq())
            .contains(0));
    EXPECT_EQ(node.grant().status, AYNI_CAPACITY_EXCEEDED);
    ASSERT_EQ(model.goal_authority.install_validated(index, saved), AYNI_OK);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, goal.result.object.id).status, AYNI_OK);
    EXPECT_EQ(node.grant().status, AYNI_CAPACITY_EXCEEDED);
    node.work.node().recover();
    EXPECT_EQ(node.grant().status, AYNI_OK);
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
    ASSERT_TRUE(KernelTestProbe::work(node.work.kernel()).tasks.find(task, index));
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).tasks.checkpoint(index).size, 0U);
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
        // ADR-0038: proposals have selected no parent binding yet, so a valid
        // parent must not turn missing child authority into a stale-binding error.
        EXPECT_EQ(node.authority(other.result.object.id).effective_reason,
                  AYNI_REASON_GOAL_AUTHORITY_MISSING);
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

// C-AUTH-032/ADR-0037: only original missing items receive bound Grants; old
// authority remains independently revocable, and retries never issue twice.
TEST(GoalApproval, fills_only_original_missing_items_and_retries_without_new_grants) {
    AuthorityWork node;
    node.count = 2;
    node.run();
    const auto generic = node.grant(0, 20);
    ASSERT_EQ(generic.status, AYNI_OK);
    node.work.node().recover();
    const auto proposed = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(proposed.status, AYNI_OK);
    const auto id = proposed.result.object.id;
    ApprovalRequest request(node.work.kernel(), id);
    ASSERT_EQ(request.body.missing_mask, 2U);
    const auto calls = node.calls;
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto approved = node.work.submit(intent);
    ASSERT_EQ(approved.status, AYNI_OK);
    EXPECT_EQ(approved.result.object.revision, proposed.result.object.revision + 1U);
    EXPECT_EQ(node.calls, calls);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 2U);
    EXPECT_EQ(node.requirement(id, 0).grant_id, generic.result.object.id);
    const auto bound = node.requirement(id, 1).grant_id;
    ASSERT_NE(bound, 0U);
    ayni_grant_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_grant(node.work.kernel(), bound, &info), AYNI_OK);
    EXPECT_EQ(info.bound_goal_id, id);
    EXPECT_EQ(info.lifetime, AYNI_GRANT_PERMANENT);
    EXPECT_EQ(info.allow_delegation, 0U);
    EXPECT_EQ(node.authority(id).effective_reason, AYNI_REASON_GRANT_NOT_DURABLE);
    const auto retry = node.work.submit(intent);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, approved.result.commit_seq);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 2U);
    node.work.node().recover();
    EXPECT_EQ(node.authority(id).missing_count, 0U);
    node.revoke(generic.result.object.id);
    EXPECT_NE(node.authority(id).missing_count, 0U);
}

// C-AUTH-032: capacity, CAS and channel failures publish no prefix or ID.
TEST(GoalApproval, insufficient_batch_capacity_and_invalid_approval_leave_the_proposal_intact) {
    AuthorityWork node;
    node.count = 2;
    node.work.node().config().grant_capacity = 1;
    node.run();
    const auto proposed = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(proposed.status, AYNI_OK);
    const auto id = proposed.result.object.id;
    ApprovalRequest request(node.work.kernel(), id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto stamp = KernelTestProbe::work(node.work.kernel()).transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 0U);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).transitions.latest().commit_seq,
              stamp.commit_seq);
    EXPECT_EQ(node.work.goal(id).lifecycle, AYNI_GOAL_PROPOSED);
    EXPECT_EQ(node.requirement(id, 0).grant_id, 0U);
    EXPECT_EQ(node.requirement(id, 1).grant_id, 0U);
    request.body.missing_mask = 1;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    request.body.missing_mask = 3;
    ++intent.expected_revision;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_REVISION_CONFLICT);
    intent.expected_revision = request.info.revision;
    intent.command_id = 0;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_INVALID_ARGUMENT);
}

// ADR-0037 decision 6: a newly revoked old choice is not silently covered by
// either a newly available generic Grant or the requested missing-item approval.
TEST(GoalApproval, a_lost_original_dependency_refuses_approval_without_reselection) {
    AuthorityWork node;
    node.count = 2;
    node.run();
    const auto first = node.grant(0, 20);
    ASSERT_EQ(first.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    node.revoke(first.result.object.id);
    const auto alternative = node.grant();
    ASSERT_EQ(alternative.status, AYNI_OK);
    node.work.node().recover();
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto before = KernelTestProbe::work(node.work.kernel()).grants.size();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), before);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, first.result.object.id);
    EXPECT_EQ(node.requirement(goal.result.object.id, 1).grant_id, 0U);
}

// C-AUTH-032: each immutable creation field participates in first acceptance
// and exact retry identity; expiry is the original duration, not time remaining.
TEST(GoalApproval, changed_echo_is_refused_and_exact_retry_survives_terminal_target_reclamation) {
    AuthorityWork node;
    node.work.node().config().goal_capacity = 1;
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE, 0, "authorized", 10000000);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    ++request.body.original.expiry_us;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    --request.body.original.expiry_us;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    node.work.node().recover();
    ++request.body.original.expiry_us;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_IDEMPOTENCY_CONFLICT);
    --request.body.original.expiry_us;
    const ayni_goal_target_v1 target{sizeof(target), 0, goal.result.object.id};
    ASSERT_EQ(node.work
                  .submit(node.work.intent(AYNI_INTENT_GOAL_CANCEL, &target,
                                           accepted.result.object.revision))
                  .status,
              AYNI_OK);
    node.work.node().recover();
    ASSERT_EQ(node.create(AYNI_TRIGGER_NONE, 0, "zero").status, AYNI_OK);
    const auto retry = node.work.submit(intent);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
    EXPECT_EQ(retry.result.object.id, accepted.result.object.id);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 1U);
}

// ADR-0037: activation anchors the trigger, never the original expiry or a
// short Grant's deadline; authority remains non-consumable until durable.
TEST(GoalApproval, activation_starts_delay_but_storage_wait_never_extends_grant_lifetime) {
    AuthorityWork node;
    node.run();
    auto body = WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1, {},
                                      AYNI_TRIGGER_DELAY);
    body.trigger_delay_us = 1000;
    body.expiry_us = 10000000;
    const auto goal = node.work.create(body);
    ASSERT_EQ(goal.status, AYNI_OK);
    const auto original_expiry = node.work.goal(goal.result.object.id).expires_us;
    node.work.node().recover();
    node.work.node().sim().advance_us(5000);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    request.body.lifetime = AYNI_GRANT_CURRENT_BOOT;
    request.body.duration_us = 10000;
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto accepted_at = node.work.node().sim().now_us();
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(node.work.goal(goal.result.object.id).next_due_us, accepted_at + 1000);
    EXPECT_EQ(node.work.goal(goal.result.object.id).expires_us, original_expiry);
    node.work.node().sim().advance_us(2000);
    node.work.node().step(1);
    EXPECT_EQ(node.work.goal(goal.result.object.id).last_task_id, 0U);
    EXPECT_TRUE(node.work.strategy().seen.empty());
    node.work.node().sim().advance_us(10000);
    node.work.node().recover();
    EXPECT_EQ(node.authority(goal.result.object.id).effective_reason, AYNI_REASON_GRANT_EXPIRED);
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// C-AUTH-032: successful durable approval reaches the existing strategy gate.
TEST(GoalApproval, durable_approval_executes_and_voice_or_expired_proposals_are_refused) {
    AuthorityWork node;
    node.run();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    uint64_t voice = 0;
    const auto desc = session_desc(AYNI_CHANNEL_VOICE);
    ASSERT_EQ(ayni_open_session(node.work.kernel(), &desc, &voice), AYNI_OK);
    intent.session_id = voice;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    intent.session_id = node.work.session();
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    node.advance_work();
    EXPECT_FALSE(node.work.strategy().seen.empty());
    const auto expiring = node.create(AYNI_TRIGGER_NONE, 0, "authorized", 1000);
    ASSERT_EQ(expiring.status, AYNI_OK);
    ApprovalRequest expired(node.work.kernel(), expiring.result.object.id);
    node.work.node().sim().advance_us(1000);
    intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &expired.body, expired.info.revision);
    intent.command_id = node.command++;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_INVALID_STATE);
}

// C-AUTH-032: the copying query never exposes a partial parameter payload.
TEST(GoalApproval, parameter_query_preserves_small_buffer_and_reports_original_duration) {
    AuthorityWork node;
    node.allow_parameters = true;
    node.run();
    const uint8_t parameters[] = {3, 1, 4};
    auto body = WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1,
                                      {parameters, sizeof(parameters)}, AYNI_TRIGGER_NONE);
    body.expiry_us = 1234;
    const auto goal = node.work.create(body);
    ASSERT_EQ(goal.status, AYNI_OK);
    uint8_t buffer[] = {99, 99, 99};
    uint32_t size = 99;
    EXPECT_EQ(ayni_get_goal_parameters(node.work.kernel(), goal.result.object.id, buffer, 2, &size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, sizeof(parameters));
    EXPECT_EQ(buffer[0], 99);
    EXPECT_EQ(ayni_get_goal_parameters(node.work.kernel(), goal.result.object.id, buffer, 3, &size),
              AYNI_OK);
    EXPECT_EQ(std::memcmp(buffer, parameters, sizeof(parameters)), 0);
    EXPECT_EQ(node.work.goal(goal.result.object.id).original_expiry_us, 1234U);
    size = 99;
    EXPECT_EQ(ayni_get_goal_parameters(node.work.kernel(), 0, buffer, 3, &size), AYNI_NOT_FOUND);
    EXPECT_EQ(size, 99U);
}

// records.md type 9/v4: strict complete canonical identity, independent of live
// catalog/Goal state, rejects truncation and preserves output on failure.
TEST(GoalApprovalCodec, canonical_round_trip_and_every_truncation_preserve_identity) {
    AuthorityWork node;
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 100;
    ayni::kernel::Intent parsed{};
    ayni::kernel::IntentFailure failure{};
    ASSERT_EQ(ayni::kernel::decode_intent(intent, parsed, failure), AYNI_OK);
    const ayni::kernel::ApprovalCommand input{parsed.expected_revision, parsed.approval};
    uint32_t size = 0;
    ASSERT_EQ(ayni::kernel::measure_approval_command(input, size), AYNI_OK);
    std::vector<uint8_t> bytes(size);
    ASSERT_EQ(ayni::kernel::encode_approval_command(input, bytes.data(), size, size), AYNI_OK);
    ayni::kernel::ApprovalCommand decoded{};
    ASSERT_TRUE(ayni::kernel::read_approval_command({bytes.data(), size}, decoded));
    EXPECT_EQ(decoded.approval.goal_id, goal.result.object.id);
    EXPECT_EQ(decoded.approval.missing_mask, 1U);
    EXPECT_EQ(decoded.approval.lifetime, AYNI_GRANT_PERMANENT);
    for (uint32_t cut = 0; cut < size; ++cut) {
        decoded.expected_revision = 99;
        EXPECT_FALSE(ayni::kernel::read_approval_command({bytes.data(), cut}, decoded));
        EXPECT_EQ(decoded.expected_revision, 99U);
    }
    bytes.push_back(0);
    EXPECT_FALSE(ayni::kernel::read_approval_command({bytes.data(), size + 1U}, decoded));
    bytes.assign(size, 99);
    EXPECT_EQ(ayni::kernel::encode_approval_command(input, bytes.data(), size - 1U, size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(bytes.front(), 99);
}

// C-AUTH-032: two missing items are distinct nondelegable roots, even if their
// requested scopes are equal; the preflight reserves the entire set.
TEST(GoalApproval, multiple_missing_items_publish_distinct_grants_in_one_transition) {
    AuthorityWork node;
    node.count = 2;
    node.high[1] = node.high[0];
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    const auto first = node.requirement(goal.result.object.id, 0).grant_id;
    const auto second = node.requirement(goal.result.object.id, 1).grant_id;
    ASSERT_NE(first, second);
    for (const auto id : {first, second}) {
        ayni_grant_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_grant(node.work.kernel(), id, &info), AYNI_OK);
        EXPECT_EQ(info.created_commit_seq, accepted.result.commit_seq);
    }
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, first);
    EXPECT_EQ(node.requirement(goal.result.object.id, 1).grant_id, second);
}

// C-STORE-019: losing power at an in-flight approval recovers the complete
// missing-item batch plus mapping, or the original proposal with no new Grants.
TEST(GoalApproval, interrupted_append_recovers_all_missing_items_and_mapping_or_neither) {
    using ayni::hosts::sim::CutAppend;
    for (const auto cut : {CutAppend::lost, CutAppend::stored}) {
        SCOPED_TRACE(static_cast<int>(cut));
        AuthorityWork node;
        node.count = 2;
        node.run();
        const auto goal = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(goal.status, AYNI_OK);
        node.work.node().recover();
        ApprovalRequest request(node.work.kernel(), goal.result.object.id);
        auto intent =
            node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
        intent.command_id = node.command++;
        const auto accepted = node.work.submit(intent);
        ASSERT_EQ(accepted.status, AYNI_OK);
        for (unsigned round = 0; round < 1000 && node.work.node().sim().pending_storage() == 0U;
             ++round) {
            const auto report = node.work.node().step(1);
            if (report.more_ready == 0U && report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.work.node().sim().now_us()) {
                node.work.node().sim().advance_us(report.next_deadline_us -
                                                  node.work.node().sim().now_us());
            }
        }
        ASSERT_NE(node.work.node().sim().pending_storage(), 0U);
        node.work.node().power_cut_and_start(cut);
        node.work.node().recover();
        ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        node.work.reopen_session();
        EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(),
                  cut == CutAppend::stored ? 2U : 0U);
        EXPECT_EQ(node.authority(goal.result.object.id).phase, cut == CutAppend::stored
                                                                   ? AYNI_GOAL_AUTHORITY_AUTHORIZED
                                                                   : AYNI_GOAL_AUTHORITY_PROPOSED);
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 2U);
        if (cut == CutAppend::stored) {
            EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
        } else {
            EXPECT_GT(retry.result.commit_seq, accepted.result.commit_seq);
        }
    }
}

// C-STORE-019/ADR-0037: FAILED and both UNKNOWN outcomes drive actual fencing
// and reload, then the original command either proves success or applies once.
TEST(GoalApproval, failed_and_unknown_appends_reload_before_retrying_the_complete_batch) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    for (const auto fault :
         {StorageFault::fail, StorageFault::unknown_lost, StorageFault::unknown_persisted}) {
        SCOPED_TRACE(static_cast<int>(fault));
        AuthorityWork node;
        node.count = 2;
        node.run();
        const auto goal = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(goal.status, AYNI_OK);
        node.work.node().recover();
        ApprovalRequest request(node.work.kernel(), goal.result.object.id);
        auto intent =
            node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
        intent.command_id = node.command++;
        const auto old_boot = node.work.node().info().boot_epoch;
        node.work.node().sim().inject_fault(StoragePort::append, fault);
        node.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        node.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        const auto accepted = node.work.submit(intent);
        ASSERT_EQ(accepted.status, AYNI_OK);
        for (unsigned round = 0; round < 5000; ++round) {
            const auto report = node.work.node().step(1);
            node.work.node().sim().complete_storage();
            if (node.work.node().info().boot_epoch > old_boot &&
                node.work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING &&
                KernelTestProbe::log(node.work.kernel()).idle()) {
                break;
            }
            if (report.more_ready == 0U && node.work.node().sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.work.node().sim().now_us()) {
                node.work.node().sim().advance_us(report.next_deadline_us -
                                                  node.work.node().sim().now_us());
            }
        }
        ASSERT_GT(node.work.node().info().boot_epoch, old_boot);
        ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        node.work.reopen_session();
        EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(),
                  fault == StorageFault::unknown_persisted ? 2U : 0U);
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 2U);
        if (fault == StorageFault::unknown_persisted) {
            EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
        } else {
            EXPECT_GT(retry.result.commit_seq, accepted.result.commit_seq);
        }
    }
}

// C-AUTH-032: changed current catalog metadata must not reinterpret the saved
// original domain. White-box drift models a registry inconsistent with creation.
TEST(GoalApproval, changed_same_version_domain_refuses_original_proposal) {
    AuthorityWork node;
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto &model = KernelTestProbe::work(node.work.kernel());
    const auto domain = model.catalogs.capabilities.domain_view(0);
    Range wider(0, 200);
    const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                             wider.field.allowed};
    uint32_t size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, const_cast<uint8_t *>(domain.data),
                                           domain.size, size),
              AYNI_OK);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto refused = node.work.submit(intent);
    EXPECT_EQ(refused.status, AYNI_DENIED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_GRANT_CATALOG_CHANGED);
    EXPECT_EQ(model.grants.size(), 0U);
    EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
}

// C-AUTH-030/032: a bound parent Grant cannot authorize its child; once expired,
// it also prevents approval of that child's independently missing authority.
TEST(GoalApproval, expired_parent_authority_refuses_child_before_autonomous_pause) {
    AuthorityWork node;
    node.run();
    const auto parent = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(parent.status, AYNI_OK);
    ApprovalRequest parent_request(node.work.kernel(), parent.result.object.id);
    parent_request.body.lifetime = AYNI_GRANT_CURRENT_BOOT;
    parent_request.body.duration_us = 1000000;
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &parent_request.body,
                                   parent_request.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    const auto child = node.create(AYNI_TRIGGER_NONE, parent.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    EXPECT_EQ(node.work.goal(child.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
    ApprovalRequest request(node.work.kernel(), child.result.object.id);
    node.work.node().sim().advance_us(parent_request.body.duration_us);
    intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 1U);
    EXPECT_EQ(node.work.goal(child.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
}

// C-AUTH-031: one remaining ID cannot fund two Grants. White-box allocator
// boundary is reached without billions of allocations; no prefix is accepted.
TEST(GoalApproval, insufficient_ids_leave_all_missing_items_and_last_id_untouched) {
    AuthorityWork node;
    node.count = 2;
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto &model = KernelTestProbe::work(node.work.kernel());
    model.ids.start(model.ids.boot_epoch(), ayni::foundation::max_id_sequence);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto before = model.transitions.latest();
    const auto refused = node.work.submit(intent);
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_ID_EXHAUSTED);
    EXPECT_EQ(model.grants.size(), 0U);
    EXPECT_TRUE(model.ids.available());
    EXPECT_EQ(model.transitions.latest().commit_seq, before.commit_seq);
    EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
}

// C-COMMIT-012/C-AUTH-032: a stale lower CAS is as invalid as a future CAS.
// White-box revision setup isolates this admission before any mutation.
TEST(GoalApproval, stale_lower_revision_and_exhausted_revision_have_no_effect) {
    for (const uint32_t revision : {2U, UINT32_MAX}) {
        AuthorityWork node;
        node.run();
        const auto goal = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(goal.status, AYNI_OK);
        ApprovalRequest request(node.work.kernel(), goal.result.object.id);
        auto &model = KernelTestProbe::work(node.work.kernel());
        uint32_t index = 0;
        ASSERT_TRUE(model.goals.find(goal.result.object.id, index));
        const_cast<ayni::work::Goal &>(model.goals.at(index)).revision = revision;
        auto intent =
            node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
        intent.command_id = node.command++;
        EXPECT_EQ(node.work.submit(intent).status, AYNI_REVISION_CONFLICT);
        if (revision == UINT32_MAX) {
            intent.expected_revision = revision;
            EXPECT_EQ(node.work.submit(intent).status, AYNI_CAPACITY_EXCEEDED);
        }
        EXPECT_EQ(model.grants.size(), 0U);
        EXPECT_EQ(model.goals.at(index).revision, revision);
        EXPECT_EQ(model.goals.at(index).lifecycle, AYNI_GOAL_PROPOSED);
    }
}

// C-AUTH-032/limits.md: exact approval storage must fit the configured parameter
// limit even when this exceeds the separate maximum Grant canonical identity.
TEST(GoalApproval, maximum_parameters_fit_exact_command_storage_and_retry) {
    AuthorityWork node;
    node.allow_parameters = true;
    node.work.node().config().goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MAX;
    node.work.node().config().storage_buffer_bytes = 0;
    node.run();
    std::vector<uint8_t> bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, 17);
    const auto body = WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1,
                                            {bytes.data(), static_cast<uint32_t>(bytes.size())});
    const auto goal = node.work.create(body);
    ASSERT_EQ(goal.status, AYNI_OK);
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    node.work.node().recover();
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), 1U);
}

// C-COMMIT-003/005 and ADR-0037: snapshot freeze refuses the whole approval
// without consuming its command. It becomes one durable batch after thaw.
TEST(GoalApproval, real_snapshot_freeze_preserves_proposal_until_atomic_approval_can_resume) {
    AuthorityWork node;
    auto &config = node.work.node().config();
    config.goal_capacity = 2;
    config.storage_buffer_bytes = 3U * smallest_storage_buffer(config);
    config.storage_log_max_bytes =
        static_cast<uint32_t>(ayni::kernel::StorageBudget::log_max_min(capacities_of(config)));
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().recover();
    ApprovalRequest request(node.work.kernel(), goal.result.object.id);
    auto intent = node.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = node.command++;
    // Real capacity failure must also leave the same command reusable.
    node.work.node().sim().set_storage_capacity(0);
    EXPECT_EQ(node.work.submit(intent).status, AYNI_STORAGE_FAILED);
    node.work.node().sim().set_storage_capacity(UINT64_MAX);
    auto &kernel = *node.work.kernel();
    auto tick = [&] {
        const auto report = node.work.node().step(1);
        node.work.node().sim().complete_storage();
        if (report.more_ready == 0U && report.next_deadline_us != UINT64_MAX &&
            report.next_deadline_us > node.work.node().sim().now_us()) {
            node.work.node().sim().advance_us(report.next_deadline_us -
                                              node.work.node().sim().now_us());
        }
    };
    bool frozen = false;
    for (unsigned round = 0; round < 1000 && !frozen; ++round) {
        const auto temporary = node.create(AYNI_TRIGGER_NONE, 0, "zero");
        ASSERT_EQ(temporary.status, AYNI_OK);
        for (unsigned step = 0; step < 10000; ++step) {
            tick();
            if (KernelTestProbe::maintenance(&kernel).active()) {
                frozen = true;
                break;
            }
            if (KernelTestProbe::log(&kernel).idle() &&
                KernelTestProbe::completions(&kernel).in_flight() == 0U) {
                break;
            }
        }
        if (frozen) {
            break;
        }
        ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, temporary.result.object.id).status,
                  AYNI_OK);
        for (unsigned step = 0; step < 10000; ++step) {
            tick();
            if (KernelTestProbe::maintenance(&kernel).active()) {
                frozen = true;
                break;
            }
            if (KernelTestProbe::log(&kernel).idle() &&
                KernelTestProbe::completions(&kernel).in_flight() == 0U) {
                break;
            }
        }
    }
    ASSERT_TRUE(frozen);
    const auto before = KernelTestProbe::work(&kernel).transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_BUSY);
    EXPECT_EQ(KernelTestProbe::work(&kernel).transitions.latest().commit_seq, before.commit_seq);
    EXPECT_EQ(KernelTestProbe::work(&kernel).grants.size(), 0U);
    EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
    for (unsigned step = 0; step < 10000 && !KernelTestProbe::maintenance(&kernel).done(); ++step) {
        tick();
    }
    ASSERT_TRUE(KernelTestProbe::maintenance(&kernel).done());
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    node.work.node().recover();
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
    EXPECT_EQ(KernelTestProbe::work(&kernel).grants.size(), 1U);
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id,
              KernelTestProbe::work(node.work.kernel()).grants.at(0).grant_id);
}

// ADR-0038: absent admission never means the same thing as explicit zero needs.
TEST(TaskAuthorityApi, zero_requirements_are_admitted_and_missing_evidence_is_reported) {
    AuthorityWork node;
    node.run();
    const auto goal = node.create(AYNI_TRIGGER_ONCE, 0, "zero");
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step();
    const auto task_id = node.work.goal(goal.result.object.id).last_task_id;
    ASSERT_NE(task_id, 0U);
    ayni_task_authority_info_v1 admitted{};
    admitted.struct_size = sizeof(admitted);
    ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), task_id, &admitted), AYNI_OK);
    EXPECT_EQ(admitted.phase, AYNI_TASK_AUTHORITY_AUTHORIZED);
    EXPECT_EQ(admitted.requirement_count, 0U);
    EXPECT_EQ(admitted.effective_reason, AYNI_REASON_NONE);
    EXPECT_EQ(admitted.binding_commit_seq,
              node.authority(goal.result.object.id).binding_commit_seq);
    EXPECT_NE(admitted.commit_seq, 0U);

    ayni_task_authority_reference_v1 unavailable{};
    std::memset(&unavailable, 0xA5, sizeof(unavailable));
    unavailable.struct_size = sizeof(unavailable);
    const auto unchanged = unavailable;
    EXPECT_EQ(ayni_get_task_authority_reference(node.work.kernel(), task_id, 0, &unavailable),
              AYNI_NOT_FOUND);
    EXPECT_EQ(std::memcmp(&unchanged, &unavailable, sizeof(unavailable)), 0);

    // White-box: emulate unavailable admission during restore, without
    // persisting the deliberately inconsistent intermediate fixture.
    auto &work = KernelTestProbe::work(node.work.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(work.tasks.find(task_id, index));
    const auto slot = work.tasks.at(index).slot;
    ayni::work::TaskAuthorityRecord saved{};
    ASSERT_TRUE(work.task_authority.read(slot, task_id, saved));
    work.task_authority.clear(slot);
    ayni_task_authority_info_v1 missing{};
    missing.struct_size = sizeof(missing);
    ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), task_id, &missing), AYNI_OK);
    EXPECT_EQ(missing.phase, AYNI_TASK_AUTHORITY_UNKNOWN);
    EXPECT_EQ(missing.requirement_count, 0U);
    EXPECT_EQ(missing.binding_commit_seq, 0U);
    EXPECT_EQ(missing.effective_reason, AYNI_REASON_GOAL_AUTHORITY_MISSING);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task_authority.install_validated(slot, saved), AYNI_OK);
}

// ADR-0038: replacement is durable before consumption, keeps the existing
// Task identity, and exact retries preserve the original generation and watermark.
TEST(GoalReauthorization, replaces_goal_and_ready_task_then_retries_without_reapplication) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step();
    const auto id = goal.result.object.id;
    const auto task = node.work.goal(id).last_task_id;
    ASSERT_NE(task, 0U);
    const auto replacement = node.grant();
    ASSERT_EQ(replacement.status, AYNI_OK);
    node.work.node().recover();
    node.revoke(original.result.object.id);
    node.advance_work();
    const auto old_task = node.work.task(task);
    const auto old_authority = node.authority(id);
    const auto calls = node.calls;
    ReauthorizationRequest request(node.work.kernel(), id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(node.calls, calls);
    EXPECT_EQ(node.authority(id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
    EXPECT_GT(node.authority(id).binding_commit_seq, old_authority.binding_commit_seq);
    EXPECT_EQ(node.authority(id).effective_reason, AYNI_REASON_GOAL_AUTHORITY_NOT_DURABLE);
    EXPECT_EQ(node.requirement(id, 0).grant_id, replacement.result.object.id);
    EXPECT_EQ(node.work.task(task).revision, old_task.revision + 1U);
    node.work.node().step(4);
    EXPECT_TRUE(node.work.strategy().seen.empty());
    const auto retry = node.work.submit(intent);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
    EXPECT_EQ(node.work.task(task).revision, old_task.revision + 1U);
    node.work.node().recover();
    node.advance_work();
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, task);
    EXPECT_EQ(node.work.goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
}

// ADR-0038: revision, authority CAS, immutable echo and complete coverage are
// checked before a single Goal or direct Task changes.
TEST(GoalReauthorization, stale_cas_echo_and_missing_coverage_publish_nothing) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(original.result.object.id);
    node.advance_work();
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto before = KernelTestProbe::work(node.work.kernel()).transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    --request.body.expected_authority_commit_seq;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_REVISION_CONFLICT);
    ++request.body.expected_authority_commit_seq;
    ++request.body.original.expiry_us;
    const auto changed = node.work.submit(intent);
    EXPECT_EQ(changed.status, AYNI_DENIED);
    EXPECT_EQ(changed.error.reason, AYNI_REASON_GOAL_REAUTHORIZATION_CHANGED);
    --request.body.original.expiry_us;
    ++intent.expected_revision;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).transitions.latest().commit_seq,
              before.commit_seq);
    EXPECT_EQ(node.authority(goal.result.object.id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
}

// ADR-0038: current durable management is required even for an exact retry;
// a newly created replacement Grant cannot be selected before persistence.
TEST(GoalReauthorization, requires_management_and_durable_existing_authority) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(original.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    node.work.node().recover();
    const auto management = intent.session_id;
    const auto voice = node.work.open_session(AYNI_CHANNEL_VOICE);
    intent.session_id = voice;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    intent.session_id = management;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    intent.session_id = voice;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    intent.session_id = management;
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
}

// ADR-0038: parent replacement immediately invalidates the child's saved
// generation, including before cleanup. Only the child's explicit command restores it.
TEST(GoalReauthorization, parent_does_not_resume_child_and_child_can_then_bind_new_parent) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto parent = node.create(AYNI_TRIGGER_NONE);
    const auto child = node.create(AYNI_TRIGGER_ONCE, parent.result.object.id);
    ASSERT_EQ(parent.status, AYNI_OK);
    ASSERT_EQ(child.status, AYNI_OK);
    node.work.node().step();
    const auto child_task = node.work.goal(child.result.object.id).last_task_id;
    ASSERT_NE(child_task, 0U);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    node.revoke(original.result.object.id);
    // No cleanup step: both authorized-phase records still refer to a gated Grant.
    ReauthorizationRequest parent_request(node.work.kernel(), parent.result.object.id);
    auto parent_intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &parent_request.body,
                                          parent_request.echo.info.revision);
    parent_intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(parent_intent).status, AYNI_OK);
    EXPECT_EQ(node.work.task(child_task).lifecycle, AYNI_TASK_PAUSED);
    node.work.node().recover();
    EXPECT_EQ(node.work.task(child_task).lifecycle, AYNI_TASK_PAUSED);
    // ADR-0038 F5: normal restart cannot turn the retained old parent
    // generation into a newly authorized child, even before cleanup scans it.
    node.work.node().reboot();
    node.work.node().recover();
    node.work.reopen_session();
    EXPECT_EQ(node.work.task(child_task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(node.work.task(child_task).hold_reason, AYNI_REASON_GOAL_AUTHORITY_PARENT);
    EXPECT_TRUE(node.work.strategy().seen.empty());
    ReauthorizationRequest child_request(node.work.kernel(), child.result.object.id);
    auto child_intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &child_request.body,
                                         child_request.echo.info.revision);
    child_intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(child_intent).status, AYNI_OK);
    node.work.node().recover();
    node.advance_work();
    EXPECT_EQ(node.work.task(child_task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(node.authority(child.result.object.id).parent_binding_commit_seq,
              node.authority(parent.result.object.id).binding_commit_seq);
}

// ADR-0038: user and policy holds are separate from authorization. The
// existing private checkpoint and step history survive the complete replacement.
TEST(GoalReauthorization, preserves_user_pause_policy_failure_steps_and_checkpoint) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step();
    node.work.strategy().answer = [](const auto &snapshot, auto &out) {
        out.intents[0] = progress_intent("saved", snapshot.revision);
        out.intent_count = 1;
        out.checkpoint[0] = 42;
        out.checkpoint_size = 1;
        return AYNI_OK;
    };
    node.work.node().step();
    node.work.strategy().answer = cannot_decide;
    node.work.node().step();
    const auto task = node.work.goal(goal.result.object.id).last_task_id;
    const auto before = node.work.task(task);
    ASSERT_EQ(before.step_count, 1U);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_PAUSE, goal.result.object.id).status, AYNI_OK);
    node.revoke(original.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const auto after = node.work.task(task);
    EXPECT_EQ(after.hold_reason, before.hold_reason);
    EXPECT_EQ(after.step_count, before.step_count);
    EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PAUSED);
    uint32_t index = 0;
    const auto &tasks = KernelTestProbe::work(node.work.kernel()).tasks;
    ASSERT_TRUE(tasks.find(task, index));
    ASSERT_EQ(tasks.checkpoint(index).size, 1U);
    EXPECT_EQ(tasks.checkpoint(index).data[0], 42U);
}

// ADR-0038: a saturated live Task revision rejects the entire replacement,
// including earlier Task rows, Goal binding and the exact-command window.
TEST(GoalReauthorization, exhausted_task_revision_refuses_whole_transaction) {
    AuthorityWork node;
    node.run();
    const auto original = node.grant();
    ASSERT_EQ(original.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step();
    const auto task = node.work.goal(goal.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    node.revoke(original.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    // White-box extreme revision setup; the ABI has no artificial revision setter.
    uint32_t index = 0;
    auto &model = KernelTestProbe::work(node.work.kernel());
    ASSERT_TRUE(model.tasks.find(task, index));
    const_cast<ayni::work::Task &>(model.tasks.at(index)).revision = UINT32_MAX;
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto before = model.transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(model.transitions.latest().commit_seq, before.commit_seq);
    EXPECT_EQ(node.authority(goal.result.object.id).commit_seq,
              request.body.expected_authority_commit_seq);
}

// ADR-0038/C-STORE-019: power loss accepts the whole replacement plus result
// or keeps the old paused Goal/Task selections. Retry works in either case.
TEST(GoalReauthorization, interrupted_append_recovers_all_or_old_admissions) {
    using ayni::hosts::sim::CutAppend;
    for (const auto cut : {CutAppend::lost, CutAppend::stored}) {
        SCOPED_TRACE(static_cast<int>(cut));
        AuthorityWork node;
        node.run();
        const auto original = node.grant();
        ASSERT_EQ(original.status, AYNI_OK);
        node.work.node().recover();
        const auto goal = node.create();
        ASSERT_EQ(goal.status, AYNI_OK);
        node.work.node().step();
        const auto id = goal.result.object.id;
        const auto task = node.work.goal(id).last_task_id;
        ASSERT_NE(task, 0U);
        node.revoke(original.result.object.id);
        node.advance_work();
        const auto replacement = node.grant();
        ASSERT_EQ(replacement.status, AYNI_OK);
        node.work.node().recover();
        ReauthorizationRequest request(node.work.kernel(), id);
        auto intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body,
                                       request.echo.info.revision);
        intent.command_id = node.command++;
        const auto accepted = node.work.submit(intent);
        ASSERT_EQ(accepted.status, AYNI_OK);
        for (unsigned round = 0; round < 1000 && node.work.node().sim().pending_storage() == 0U;
             ++round) {
            const auto report = node.work.node().step(1);
            if (report.more_ready == 0U && report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.work.node().sim().now_us()) {
                node.work.node().sim().advance_us(report.next_deadline_us -
                                                  node.work.node().sim().now_us());
            }
        }
        ASSERT_NE(node.work.node().sim().pending_storage(), 0U);
        node.work.node().power_cut_and_start(cut);
        node.work.node().recover();
        ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        node.work.reopen_session();
        const auto selected = node.requirement(id, 0).grant_id;
        EXPECT_EQ(selected, cut == CutAppend::stored ? replacement.result.object.id
                                                     : original.result.object.id);
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(node.work.goal(id).last_task_id, task);
        if (cut == CutAppend::stored) {
            EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
        } else {
            EXPECT_GT(retry.result.commit_seq, accepted.result.commit_seq);
        }
    }
}

// ADR-0038: retry returns the original resume boundary. Queued old input is
// skipped only for the restored Goal; a shared source's other Goal keeps it.
TEST(GoalReauthorization, input_frontier_is_goal_local_and_not_resampled_on_retry) {
    AuthorityWork node;
    node.run();
    const auto old_grant = node.grant();
    ASSERT_EQ(old_grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_LOCAL_INPUT);
    const auto other = node.create(AYNI_TRIGGER_LOCAL_INPUT, 0, "zero");
    ASSERT_EQ(goal.status, AYNI_OK);
    ASSERT_EQ(other.status, AYNI_OK);
    node.revoke(old_grant.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const ayni_event_v1 event{sizeof(event), AYNI_EVENT_LOCAL_INPUT, text("button")};
    ASSERT_EQ(ayni_post_event(node.work.kernel(), &event), AYNI_OK);
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    ASSERT_EQ(ayni_post_event(node.work.kernel(), &event), AYNI_OK);
    ASSERT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
    node.work.node().recover();
    node.advance_work();
    EXPECT_EQ(node.work.goal(goal.result.object.id).occurrences, 1U);
    EXPECT_EQ(node.work.goal(other.result.object.id).occurrences, 2U);
}

// ADR-0038: recurring resume skips even backlog predating the deny, then
// rejoins the original rhythm rather than restarting a whole interval.
TEST(GoalReauthorization, interval_resume_skips_all_undeveloped_due_occurrences) {
    AuthorityWork node;
    node.run();
    const auto old_grant = node.grant();
    ASSERT_EQ(old_grant.status, AYNI_OK);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().sim().advance_us(5U * AYNI_TRIGGER_PERIOD_MIN_US);
    node.revoke(old_grant.result.object.id);
    // Before autonomous cleanup: backlog still lives in the original cursor.
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    const auto after = node.work.goal(goal.result.object.id);
    EXPECT_EQ(after.occurrences_skipped, 6U); // Includes the occurrence due at creation.
    EXPECT_EQ(after.last_task_id, 0U);
    node.work.node().recover();
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    node.advance_work();
    EXPECT_EQ(node.work.strategy().seen.size(), 1U);
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// ADR-0038: explicit zero requirements remain a real replacement fact. Its
// parent nonce must be restored even though it has no Grant references.
TEST(GoalReauthorization, zero_requirement_child_gets_explicit_new_admission) {
    AuthorityWork node;
    node.run();
    const auto old_grant = node.grant();
    ASSERT_EQ(old_grant.status, AYNI_OK);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto parent = node.create(AYNI_TRIGGER_NONE);
    const auto child = node.create(AYNI_TRIGGER_NONE, parent.result.object.id, "zero");
    ASSERT_EQ(parent.status, AYNI_OK);
    ASSERT_EQ(child.status, AYNI_OK);
    node.revoke(old_grant.result.object.id);
    for (const auto id : {parent.result.object.id, child.result.object.id}) {
        ReauthorizationRequest request(node.work.kernel(), id);
        auto intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body,
                                       request.echo.info.revision);
        intent.command_id = node.command++;
        ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
        node.work.node().recover();
    }
    EXPECT_EQ(node.authority(child.result.object.id).requirement_count, 0U);
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// ADR-0038: multiple direct live Tasks receive the same new generation in
// one transition, while already terminal Task history keeps its old selection.
TEST(GoalReauthorization, all_direct_live_tasks_bind_together_and_terminal_history_stays) {
    AuthorityWork node;
    node.run();
    const auto old_grant = node.grant();
    ASSERT_EQ(old_grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(goal.status, AYNI_OK);
    const auto id = goal.result.object.id;
    std::vector<uint64_t> tasks;
    for (unsigned round = 0; round < 3; ++round) {
        if (round != 0U) {
            node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
        }
        // Due derivation precedes READY decisions in a one-item step (ADR-0025).
        node.work.node().step(1);
        node.work.node().recover();
        const auto task = node.work.goal(id).last_task_id;
        ASSERT_NE(task, 0U);
        ASSERT_TRUE(tasks.empty() || tasks.back() != task);
        tasks.push_back(task);
    }
    // Complete the first Task; the two later READY Tasks remain live.
    node.work.node().step(1);
    ASSERT_EQ(node.work.task(tasks.front()).lifecycle, AYNI_TASK_COMPLETED);
    node.work.node().recover();
    node.revoke(old_grant.result.object.id);
    node.advance_work();
    const auto replacement = node.grant();
    ASSERT_EQ(replacement.status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    for (const auto task : tasks) {
        ayni_task_authority_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), task, &info), AYNI_OK);
        ayni_task_authority_reference_v1 reference{};
        reference.struct_size = sizeof(reference);
        ASSERT_EQ(ayni_get_task_authority_reference(node.work.kernel(), task, 0, &reference),
                  AYNI_OK);
        EXPECT_EQ(reference.grant_id, task == tasks.front() ? old_grant.result.object.id
                                                            : replacement.result.object.id);
        if (task != tasks.front()) {
            EXPECT_EQ(info.binding_commit_seq, accepted.result.commit_seq);
            EXPECT_EQ(info.commit_seq, accepted.result.commit_seq);
        }
    }
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.work.task(tasks.front()).lifecycle, AYNI_TASK_COMPLETED);
}

// ADR-0038: FAILED/UNKNOWN writes fence and reload the existing coordinator;
// persisted results retry exactly, lost ones reapply to the old complete facts.
TEST(GoalReauthorization, failed_or_unknown_append_preserves_atomic_recovery_and_retry) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    for (const auto fault :
         {StorageFault::fail, StorageFault::unknown_lost, StorageFault::unknown_persisted}) {
        SCOPED_TRACE(static_cast<int>(fault));
        AuthorityWork node;
        node.run();
        const auto old_grant = node.grant();
        ASSERT_EQ(old_grant.status, AYNI_OK);
        node.work.node().recover();
        const auto goal = node.create(AYNI_TRIGGER_NONE);
        ASSERT_EQ(goal.status, AYNI_OK);
        node.revoke(old_grant.result.object.id);
        node.advance_work();
        const auto replacement = node.grant();
        ASSERT_EQ(replacement.status, AYNI_OK);
        node.work.node().recover();
        ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
        auto intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body,
                                       request.echo.info.revision);
        intent.command_id = node.command++;
        const auto boot = node.work.node().info().boot_epoch;
        node.work.node().sim().inject_fault(StoragePort::append, fault);
        node.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        node.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        const auto accepted = node.work.submit(intent);
        ASSERT_EQ(accepted.status, AYNI_OK);
        for (unsigned round = 0; round < 5000; ++round) {
            const auto report = node.work.node().step(1);
            node.work.node().sim().complete_storage();
            if (node.work.node().info().boot_epoch > boot &&
                node.work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING &&
                KernelTestProbe::log(node.work.kernel()).idle()) {
                break;
            }
            if (report.more_ready == 0U && node.work.node().sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.work.node().sim().now_us()) {
                node.work.node().sim().advance_us(report.next_deadline_us -
                                                  node.work.node().sim().now_us());
            }
        }
        ASSERT_GT(node.work.node().info().boot_epoch, boot);
        ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        node.work.reopen_session();
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id,
                  replacement.result.object.id);
        if (fault == StorageFault::unknown_persisted) {
            EXPECT_EQ(retry.result.commit_seq, accepted.result.commit_seq);
        } else {
            EXPECT_GT(retry.result.commit_seq, accepted.result.commit_seq);
        }
    }
}

// ADR-0038: dedicated complete coverage wins without re-exporting the ceiling.
TEST(GoalReauthorization, dedicated_grant_wins_for_every_completely_covered_requirement) {
    AuthorityWork node;
    node.count = 2;
    node.run();
    const auto generic = node.grant(0, 20);
    ASSERT_EQ(generic.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    // A valid generic candidate precedes the dedicated root. Approval still
    // fills only the originally missing item; selection must prefer the later root.
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ApprovalRequest approval(node.work.kernel(), goal.result.object.id);
    ASSERT_EQ(approval.body.missing_mask, 2U);
    auto approve =
        node.work.intent(AYNI_INTENT_GOAL_APPROVE, &approval.body, approval.info.revision);
    approve.command_id = node.command++;
    ASSERT_EQ(node.work.submit(approve).status, AYNI_OK);
    node.work.node().recover();
    const auto dedicated = node.requirement(goal.result.object.id, 1).grant_id;
    ASSERT_NE(dedicated, 0U);
    node.revoke(generic.result.object.id);
    node.advance_work();
    node.count = 0; // Exporter drift must never shrink the saved requirements.
    const auto calls = node.calls;
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(node.calls, calls);
    EXPECT_EQ(node.authority(goal.result.object.id).requirement_count, 2U);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, dedicated);
    EXPECT_EQ(node.requirement(goal.result.object.id, 1).grant_id, dedicated);
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// ADR-0038: partial ranges cannot be unioned, and absence of one complete
// selection refuses the entire transaction. Same-category choices use the least ID.
TEST(GoalReauthorization, never_unions_grants_and_selects_lowest_id_within_each_category) {
    AuthorityWork node;
    node.count = 2;
    node.run();
    const auto old = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(old.result.object.id);
    node.advance_work();
    const auto narrow = node.grant(0, 20);
    ASSERT_EQ(narrow.status, AYNI_OK);
    ASSERT_EQ(node.grant(21, 100).status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto before = node.authority(goal.result.object.id);
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    EXPECT_EQ(node.authority(goal.result.object.id).commit_seq, before.commit_seq);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, old.result.object.id);
    const auto complete = node.grant();
    ASSERT_EQ(complete.status, AYNI_OK);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, narrow.result.object.id);
    EXPECT_EQ(node.requirement(goal.result.object.id, 1).grant_id, complete.result.object.id);
}

// ADR-0038: exact retries precede object lookup after Goal reclamation. Altered
// original CAS/echo conflicts, and the replacement slot owner's revision stays intact.
TEST(GoalReauthorization, exact_retry_survives_goal_reclamation_and_changed_echo_conflicts) {
    AuthorityWork node;
    node.work.node().config().goal_capacity = 1;
    node.run();
    const auto old = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(old.result.object.id);
    node.advance_work();
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    node.work.node().recover();
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_CANCEL, goal.result.object.id).status, AYNI_OK);
    node.work.node().recover();
    const auto replacement = node.create(AYNI_TRIGGER_NONE, 0, "zero");
    ASSERT_EQ(replacement.status, AYNI_OK);
    ayni_goal_info_v1 gone{};
    gone.struct_size = sizeof(gone);
    EXPECT_EQ(ayni_get_goal(node.work.kernel(), goal.result.object.id, &gone), AYNI_NOT_FOUND);
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
    ++request.body.expected_authority_commit_seq;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_IDEMPOTENCY_CONFLICT);
    --request.body.expected_authority_commit_seq;
    ++request.body.original.expiry_us;
    EXPECT_EQ(node.work.submit(intent).status, AYNI_IDEMPOTENCY_CONFLICT);
    EXPECT_EQ(node.work.goal(replacement.result.object.id).revision, 1U);
}

// ADR-0038: same-boot resume keeps the original delay and an overdue ONCE
// executes exactly once, provided its Goal has not itself expired.
TEST(GoalReauthorization, delayed_and_once_goals_keep_their_original_same_boot_schedule) {
    for (const auto trigger : {AYNI_TRIGGER_DELAY, AYNI_TRIGGER_ONCE}) {
        SCOPED_TRACE(trigger);
        AuthorityWork node;
        node.run();
        const auto old = node.grant();
        ASSERT_EQ(old.status, AYNI_OK);
        ASSERT_EQ(node.grant().status, AYNI_OK);
        node.work.node().recover();
        constexpr uint64_t delay = 10U * AYNI_TRIGGER_PERIOD_MIN_US;
        auto body =
            WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1, {}, trigger);
        body.trigger_delay_us = trigger == AYNI_TRIGGER_DELAY ? delay : 0U;
        body.expiry_us = 3U * delay;
        const auto start = node.work.node().sim().now_us();
        const auto goal = node.work.create(body);
        ASSERT_EQ(goal.status, AYNI_OK);
        node.revoke(old.result.object.id);
        node.advance_work();
        node.work.node().sim().advance_us(delay / 2U);
        ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
        auto intent = node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body,
                                       request.echo.info.revision);
        intent.command_id = node.command++;
        ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
        node.work.node().recover();
        if (trigger == AYNI_TRIGGER_DELAY) {
            node.advance_work();
            EXPECT_TRUE(node.work.strategy().seen.empty());
            const auto due = start + delay;
            ASSERT_LT(node.work.node().sim().now_us(), due);
            node.work.node().sim().advance_us(due - node.work.node().sim().now_us());
        }
        node.advance_work();
        ASSERT_EQ(node.work.strategy().seen.size(), 1U);
        EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
        node.work.node().sim().advance_us(delay);
        node.advance_work();
        EXPECT_EQ(node.work.strategy().seen.size(), 1U);
    }
}

// ADR-0038: canonical reauthorization retains both CAS values and the full
// original echo. Malformed byte input never publishes a partially decoded identity.
TEST(GoalReauthorizationCodec, exact_identity_round_trip_and_all_truncations_are_atomic) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    constexpr uint64_t high_sequence = (uint64_t{1} << 40U) + 19U;
    request.body.expected_authority_commit_seq = high_sequence;
    auto raw =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    raw.command_id = node.command++;
    ayni::kernel::Intent decoded{};
    ayni::kernel::IntentFailure failure{};
    ASSERT_EQ(ayni::kernel::decode_intent(raw, decoded, failure), AYNI_OK);
    const ayni::kernel::ReauthorizationCommand original{decoded.expected_revision,
                                                        decoded.reauthorization};
    uint32_t size = 0;
    ASSERT_EQ(ayni::kernel::measure_reauthorization_command(original, size), AYNI_OK);
    std::vector<uint8_t> bytes(size + 1U);
    ASSERT_EQ(ayni::kernel::encode_reauthorization_command(original, bytes.data(), size, size),
              AYNI_OK);
    ayni::kernel::ReauthorizationCommand restored{};
    ASSERT_TRUE(ayni::kernel::read_reauthorization_command({bytes.data(), size}, restored));
    EXPECT_EQ(restored.reauthorization.goal_id, goal.result.object.id);
    EXPECT_EQ(restored.reauthorization.expected_authority_commit_seq,
              request.body.expected_authority_commit_seq);
    EXPECT_EQ(restored.expected_revision, request.echo.info.revision);
    for (uint32_t cut = 0; cut <= bytes.size(); ++cut) {
        if (cut == size) {
            continue;
        }
        restored.expected_revision = 77;
        restored.reauthorization.goal_id = 99;
        restored.reauthorization.expected_authority_commit_seq = 999;
        EXPECT_FALSE(ayni::kernel::read_reauthorization_command({bytes.data(), cut}, restored));
        EXPECT_EQ(restored.expected_revision, 77U);
        EXPECT_EQ(restored.reauthorization.goal_id, 99U);
        EXPECT_EQ(restored.reauthorization.expected_authority_commit_seq, 999U);
    }
    constexpr uint32_t authority_cas_at =
        ayni::kernel::reauthorization_command_prefix_size - sizeof(uint64_t);
    ayni::foundation::store_le64(bytes.data() + authority_cas_at, 0);
    EXPECT_FALSE(ayni::kernel::read_reauthorization_command({bytes.data(), size}, restored));
}

// ADR-0038: the shared command/storage coordinator refuses a complete
// reauthorization without changing its Goal revision, ceiling or command identity.
TEST(GoalReauthorization, pinned_window_and_storage_shortage_refuse_without_partial_binding) {
    AuthorityWork node;
    node.work.node().config().command_capacity = 1;
    node.run();
    const auto old = node.grant();
    const auto replacement = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    ASSERT_EQ(replacement.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(old.result.object.id);
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto pinned = node.work.submit(intent);
    EXPECT_EQ(pinned.status, AYNI_BUSY);
    EXPECT_EQ(pinned.error.reason, AYNI_REASON_COMMAND_PINNED);
    node.work.node().recover();
    node.work.node().sim().set_storage_capacity(0);
    const auto short_space = node.work.submit(intent);
    EXPECT_EQ(short_space.status, AYNI_STORAGE_FAILED);
    EXPECT_EQ(node.work.goal(goal.result.object.id).revision, request.echo.info.revision);
    EXPECT_EQ(node.authority(goal.result.object.id).commit_seq,
              request.body.expected_authority_commit_seq);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, old.result.object.id);
    node.work.node().sim().set_storage_capacity(UINT64_MAX);
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id, replacement.result.object.id);
}

// ADR-0038: an expired Goal cannot be revived, even before its autonomous
// expiry transition has run and while replacement authority is available.
TEST(GoalReauthorization, own_goal_expiry_refuses_before_autonomous_cleanup) {
    AuthorityWork node;
    node.run();
    const auto old = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create(AYNI_TRIGGER_NONE, 0, "authorized", AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(goal.status, AYNI_OK);
    node.revoke(old.result.object.id);
    ReauthorizationRequest request(node.work.kernel(), goal.result.object.id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto before = KernelTestProbe::work(node.work.kernel()).transitions.latest();
    node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    EXPECT_EQ(node.work.submit(intent).status, AYNI_INVALID_STATE);
    EXPECT_EQ(KernelTestProbe::work(node.work.kernel()).transitions.latest().commit_seq,
              before.commit_seq);
    EXPECT_EQ(node.authority(goal.result.object.id).commit_seq,
              request.body.expected_authority_commit_seq);
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// C-AUTH-034: ordinary resume may restore user lifecycle while independent
// authorization still denies consumption. Explicit reauthorization then runs
// the same Task instead of retaining a pause invented by propagation.
TEST(GoalReauthorization,
     ordinary_resume_before_reauthorization_preserves_independent_pause_causes) {
    AuthorityWork node;
    node.run();
    const auto old = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().step(1);
    const auto goal_id = created.result.object.id;
    const auto task_id = node.work.goal(goal_id).last_task_id;
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    node.revoke(old.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.authority(goal_id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    ASSERT_EQ(node.work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_PAUSED); // Independent deny remains.
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), goal_id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    node.advance_work();
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, task_id);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// C-AUTH-030 / ADR-0038: permanent complete admissions continue the same
// READY Task after a normal restart, including private progress and checkpoint.
TEST(WorkAuthorityRestart, permanent_admission_continues_the_same_task_checkpoint_and_steps) {
    AuthorityWork node;
    node.run();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().step(1);
    node.work.strategy().answer = [](const auto &task, auto &out) {
        out.intents[0] = progress_intent("saved", task.revision);
        out.intent_count = 1;
        out.checkpoint[0] = 42;
        out.checkpoint_size = 1;
        return AYNI_OK;
    };
    node.work.node().step(1);
    node.work.node().recover();
    const auto goal_id = created.result.object.id;
    const auto task_id = node.work.goal(goal_id).last_task_id;
    const auto before = node.work.task(task_id);
    const auto binding = node.authority(goal_id).binding_commit_seq;
    ASSERT_EQ(before.step_count, 1U);
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(node.work.task(task_id).revision, before.revision);
    EXPECT_EQ(node.work.task(task_id).commit_seq, before.commit_seq);
    EXPECT_EQ(node.authority(goal_id).binding_commit_seq, binding);
    node.work.strategy().seen.clear();
    node.work.strategy().answer = {};
    node.work.node().step(1);
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    const auto &seen = node.work.strategy().seen.front();
    EXPECT_EQ(seen.snapshot.task_id, task_id);
    EXPECT_EQ(seen.checkpoint, (std::vector<uint8_t>{42}));
    EXPECT_EQ(seen.steps, (std::vector<std::string>{"saved"}));
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// C-AUTH-007 / ADR-0038: a new boot cannot prove an old monotonic lease.
// Existing READY work is denied before the bounded cleanup reaches it, and
// making a permanent Grant available still requires explicit reauthorization.
TEST(WorkAuthorityRestart, old_boot_timed_authority_requires_explicit_reauthorization) {
    AuthorityWork node;
    node.run();
    const auto timed = node.grant(0, 100, 100ULL * AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(timed.status, AYNI_OK);
    node.work.node().recover();
    const auto created = node.create();
    ASSERT_EQ(created.status, AYNI_OK);
    node.work.node().step(1);
    node.work.node().recover();
    const auto goal_id = created.result.object.id;
    const auto task_id = node.work.goal(goal_id).last_task_id;
    const auto before = node.work.task(task_id);
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_NE(node.work.task(task_id).hold_reason, AYNI_REASON_NONE);
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    EXPECT_EQ(node.work.task(task_id).revision, before.revision);
    node.work.reopen_session();
    const auto replacement = node.grant();
    ASSERT_EQ(replacement.status, AYNI_OK);
    node.work.node().recover();
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    ReauthorizationRequest request(node.work.kernel(), goal_id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    node.advance_work();
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, task_id);
}

// ADR-0038: maximum requirements and direct READY Task cohort must fit one
// atomic replacement and survive restart without copying Scope into each Task.
TEST(WorkAuthorityRestart, maximum_requirement_and_task_cohort_survives_replacement_and_restart) {
    AuthorityWork node;
    node.count = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    node.allow_parameters = true;
    auto &config = node.work.node().config();
    config.task_capacity = AYNI_TASK_CAPACITY_MAX;
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MAX;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    node.run();
    const auto old = node.grant();
    ASSERT_EQ(old.status, AYNI_OK);
    node.work.node().recover();
    const std::vector<uint8_t> parameters(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, 42);
    auto body = WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1,
                                      {parameters.data(), static_cast<uint32_t>(parameters.size())},
                                      AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
    const auto created = node.work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    const auto goal_id = created.result.object.id;
    std::vector<uint64_t> tasks;
    for (uint32_t at = 0; at < AYNI_TASK_CAPACITY_MAX; ++at) {
        if (at != 0U) {
            node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
        }
        node.work.node().step(1);
        const auto task_id = node.work.goal(goal_id).last_task_id;
        ASSERT_NE(task_id, 0U);
        ASSERT_TRUE(tasks.empty() || tasks.back() != task_id);
        tasks.push_back(task_id);
        node.work.node().recover();
    }
    ASSERT_TRUE(node.work.strategy().seen.empty());
    node.revoke(old.result.object.id);
    node.advance_work();
    const auto replacement = node.grant();
    ASSERT_EQ(replacement.status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), goal_id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    for (const auto id : tasks) {
        ayni_task_authority_info_v1 admission{};
        admission.struct_size = sizeof(admission);
        ASSERT_EQ(ayni_get_task_authority(node.work.kernel(), id, &admission), AYNI_OK);
        EXPECT_EQ(admission.requirement_count, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);
        EXPECT_EQ(admission.binding_commit_seq, accepted.result.commit_seq);
    }
    node.work.node().recover();
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    for (const auto id : tasks) {
        EXPECT_EQ(node.work.task(id).lifecycle, AYNI_TASK_READY);
        ayni_task_authority_reference_v1 reference{};
        reference.struct_size = sizeof(reference);
        ASSERT_EQ(ayni_get_task_authority_reference(
                      node.work.kernel(), id, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX - 1U, &reference),
                  AYNI_OK);
        EXPECT_EQ(reference.grant_id, replacement.result.object.id);
    }
    node.work.strategy().answer = {};
    for (unsigned round = 0; round < 32U && node.work.strategy().seen.empty(); ++round) {
        node.work.node().step(1); // The persisted cleanup request precedes decisions.
    }
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, tasks.front());
    EXPECT_EQ(node.work.strategy().seen.front().parameters, parameters);
}

// C-AUTH-030 / C-AUTH-034: an independent authorization pause survives boot
// without being rewritten into an ordinary Task pause that reauthorization
// cannot clear. The original READY Task resumes after the new binding is durable.
TEST(WorkAuthorityRestart, revoked_ready_task_resumes_after_restart_and_explicit_reauthorization) {
    AuthorityWork node;
    node.run();
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    const auto goal = node.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    node.work.node().step(1);
    const auto goal_id = goal.result.object.id;
    const auto task_id = node.work.goal(goal_id).last_task_id;
    ASSERT_NE(task_id, 0U);
    node.revoke(grant.result.object.id);
    node.advance_work();
    const auto before = node.work.task(task_id);
    ASSERT_EQ(node.authority(goal_id).phase, AYNI_GOAL_AUTHORITY_PAUSED);
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.task(task_id).revision, before.revision);
    EXPECT_EQ(node.work.task(task_id).commit_seq, before.commit_seq);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    node.advance_work();
    EXPECT_TRUE(node.work.strategy().seen.empty());
    node.work.reopen_session();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), goal_id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.work.node().recover();
    node.advance_work();
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, task_id);
    EXPECT_EQ(node.work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// ADR-0038: the minimum-work arena profile admits an explicit zero-needs
// Goal/Task and normally restores it. Each configured work table has one slot.
TEST(WorkAuthorityRestart, minimum_work_capacities_restore_explicit_zero_admission) {
    WorkNode work;
    auto &config = work.node().config();
    config.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    config.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    config.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    config.agent_capacity = 1;
    config.goal_type_capacity = 1;
    config.goal_capacity = 1;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MIN;
    config.session_capacity = 1;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MIN;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_MIN;
    config.task_capacity = 1;
    config.strategy_capacity = 1;
    config.policy_intent_capacity = 1;
    config.task_step_capacity = 1;
    config.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN;
    config.command_capacity = 1;
    config.capability_capacity = 1;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    config.grant_capacity = 1;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN;
    config.goal_requirement_capacity = 1;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    work.node().on_create([&](ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy(), 1, 0);
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        const auto type = goal_type_desc("zero", 1);
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    });
    work.run();
    const auto created = work.create(
        WorkNode::create_body(work.instance(agent_a), 0, "zero", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    work.node().step(1);
    const auto task_id = work.goal(created.result.object.id).last_task_id;
    ASSERT_NE(task_id, 0U);
    work.node().recover();
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
    work.node().step(1);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.strategy().seen.front().snapshot.task_id, task_id);
    EXPECT_EQ(work.goal(created.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
}

// C-AUTH-030 / ADR-0038: catalog absence or exact-domain drift holds the
// actual READY consumer without rewriting its immutable evidence or exporting
// new needs. A compatible later boot continues that same Task.
TEST(WorkAuthorityRestart, catalog_incompatibility_keeps_ready_progress_until_a_compatible_boot) {
    for (const bool missing : {false, true}) {
        for (const bool derived : {false, true}) {
            SCOPED_TRACE(missing);
            SCOPED_TRACE(derived);
            AuthorityWork source;
            source.run();
            ASSERT_EQ(source.grant().status, AYNI_OK);
            ASSERT_EQ(source.grant(0, 100, 10000000).status, AYNI_OK);
            source.work.node().recover();
            const auto goal = source.create();
            ASSERT_EQ(goal.status, AYNI_OK);
            if (derived) {
                source.work.node().step(1);
            }
            source.work.node().recover();
            const auto goal_id = goal.result.object.id;
            const auto task_id = source.work.goal(goal_id).last_task_id;
            AuthorityWork incompatible;
            incompatible.capability_available = !missing;
            incompatible.domain_max = 200;
            incompatible.work.node().sim().durable_storage() =
                source.work.node().sim().durable_storage();
            incompatible.run();
            ASSERT_EQ(incompatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
            EXPECT_EQ(incompatible.work.goal(goal_id).hold_reason,
                      missing ? AYNI_REASON_CAPABILITY_UNKNOWN : AYNI_REASON_GRANT_CATALOG_CHANGED);
            if (derived) {
                EXPECT_EQ(incompatible.work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
            }
            incompatible.advance_work();
            EXPECT_EQ(incompatible.work.goal(goal_id).last_task_id, task_id);
            EXPECT_TRUE(incompatible.work.strategy().seen.empty());
            EXPECT_EQ(incompatible.calls, 0U);
            EXPECT_EQ(incompatible.authority(goal_id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
            EXPECT_EQ(incompatible.work.node().step(1).more_ready, 0U);
            AuthorityWork compatible;
            compatible.work.node().sim().durable_storage() =
                incompatible.work.node().sim().durable_storage();
            compatible.run();
            ASSERT_EQ(compatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
            if (derived) {
                EXPECT_EQ(compatible.work.task(task_id).lifecycle, AYNI_TASK_READY);
            }
            compatible.advance_work();
            ASSERT_EQ(compatible.work.strategy().seen.size(), 1U);
            EXPECT_EQ(compatible.work.strategy().seen.front().snapshot.goal_id, goal_id);
            if (derived) {
                EXPECT_EQ(compatible.work.strategy().seen.front().snapshot.task_id, task_id);
            }
            EXPECT_EQ(compatible.calls, 0U);
        }
    }
}

// C-AUTH-020 / ADR-0038: the selected delegated Grant can be compatible while
// its parent has an unavailable registered subject. The Grant owner checks the
// whole path's cached compatibility; it never becomes a persistent work deny.
TEST(WorkAuthorityRestart, ancestor_grant_subject_hold_does_not_become_a_persistent_work_pause) {
    AuthorityWork source;
    source.run();
    Range range(0, 100);
    const ayni_grant_create_v1 parent_body{sizeof(parent_body),           1, text("device.move"),
                                           source.work.instance(agent_b), 0, AYNI_GRANT_PERMANENT,
                                           AYNI_GRANT_ALLOW_DELEGATION,   0, range.scope};
    auto parent_intent = source.work.intent(AYNI_INTENT_GRANT_CREATE, &parent_body);
    parent_intent.command_id = source.command++;
    const auto parent = source.work.submit(parent_intent);
    ASSERT_EQ(parent.status, AYNI_OK);
    source.work.node().recover();
    auto child_body = parent_body;
    child_body.subject_instance_id = source.work.instance(agent_a);
    child_body.parent_grant_id = parent.result.object.id;
    child_body.flags = 0;
    auto child_intent = source.work.intent(AYNI_INTENT_GRANT_CREATE, &child_body, 1);
    child_intent.command_id = source.command++;
    ASSERT_EQ(source.work.submit(child_intent).status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.create();
    ASSERT_EQ(goal.status, AYNI_OK);
    source.work.node().step(1);
    source.work.node().recover();
    const auto goal_id = goal.result.object.id;
    const auto task_id = source.work.goal(goal_id).last_task_id;
    AuthorityWork unavailable;
    unavailable.agent_b_available = false;
    unavailable.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    unavailable.run();
    ASSERT_EQ(unavailable.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    unavailable.advance_work();
    EXPECT_TRUE(unavailable.work.strategy().seen.empty());
    EXPECT_EQ(unavailable.work.task(task_id).hold_reason, AYNI_REASON_GRANT_SUBJECT);
    EXPECT_EQ(unavailable.authority(goal_id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
    EXPECT_EQ(unavailable.work.node().step(1).more_ready, 0U);
    AuthorityWork restored;
    restored.work.node().sim().durable_storage() = unavailable.work.node().sim().durable_storage();
    restored.run();
    ASSERT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    restored.advance_work();
    ASSERT_EQ(restored.work.strategy().seen.size(), 1U);
    EXPECT_EQ(restored.work.strategy().seen.front().snapshot.task_id, task_id);
}

// ADR-0038 resource gate (White-box timing only): maximal retained Grants,
// delegation/Goal depth and requirements share one READY cohort. Measure real
// Native readiness-query wall time, separate from the simulated Host clock;
// no timing assertion.
TEST(WorkAuthorityRestart, full_grant_table_and_deep_shared_cohort_have_bounded_readiness_work) {
    AuthorityWork node;
    node.count = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    auto &config = node.work.node().config();
    config.grant_capacity = AYNI_GRANT_CAPACITY_MAX;
    config.command_capacity = AYNI_COMMAND_CAPACITY_MAX;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX;
    config.goal_capacity = AYNI_GOAL_DEPTH_MAX_MAX + 1U;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MAX;
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    config.task_capacity = AYNI_TASK_CAPACITY_MAX;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    node.run();
    Range range(0, 100);
    ayni_grant_create_v1 body{sizeof(body),
                              1,
                              text("device.move"),
                              node.work.instance(agent_b),
                              0,
                              AYNI_GRANT_PERMANENT,
                              0,
                              0,
                              range.scope};
    const uint32_t chain_nodes = AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX + 1U;
    for (uint32_t at = 0; at < AYNI_GRANT_CAPACITY_MAX - chain_nodes; ++at) {
        auto intent = node.work.intent(AYNI_INTENT_GRANT_CREATE, &body);
        intent.command_id = node.command++;
        ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
        node.work.node().recover();
    }
    uint64_t parent_grant = 0;
    for (uint32_t depth = 0; depth < chain_nodes; ++depth) {
        const bool leaf = depth + 1U == chain_nodes;
        body.parent_grant_id = parent_grant;
        body.subject_instance_id = node.work.instance(leaf ? agent_a : agent_b);
        body.flags = leaf ? 0U : AYNI_GRANT_ALLOW_DELEGATION;
        auto intent = node.work.intent(AYNI_INTENT_GRANT_CREATE, &body, depth == 0U ? 0U : 1U);
        intent.command_id = node.command++;
        const auto created = node.work.submit(intent);
        ASSERT_EQ(created.status, AYNI_OK);
        parent_grant = created.result.object.id;
        node.work.node().recover();
    }
    uint64_t goal_id = 0;
    for (uint32_t depth = 0; depth <= AYNI_GOAL_DEPTH_MAX_MAX; ++depth) {
        const auto created = node.create(
            depth == AYNI_GOAL_DEPTH_MAX_MAX ? AYNI_TRIGGER_INTERVAL : AYNI_TRIGGER_NONE, goal_id);
        ASSERT_EQ(created.status, AYNI_OK);
        goal_id = created.result.object.id;
        node.work.node().recover();
    }
    auto &model = KernelTestProbe::work(node.work.kernel());
    uint32_t goal_index = 0;
    ASSERT_TRUE(model.goals.find(goal_id, goal_index));
    ASSERT_EQ(model
                  .check_goal_authority(goal_index, node.work.node().sim().now_us(),
                                        node.work.node().info().durable_seq)
                  .status,
              AYNI_OK);
    auto tables = model.tables();
    // White-box resource fixture: the real C ABI admitted all immutable
    // Goal/Grant facts. Construct the unchanged cohort through its owners
    // without running the readiness query during every setup step; no claim
    // about durability is made for these synthetic Task transitions.
    for (uint32_t at = 0; at < AYNI_TASK_CAPACITY_MAX; ++at) {
        if (at != 0U) {
            node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
        }
        uint64_t task_id = 0;
        ASSERT_TRUE(model.ids.allocate(task_id));
        const auto stamp = model.transitions.next();
        ayni::work::derive(tables, goal_index, node.work.node().sim().now_us(), task_id, stamp);
        ASSERT_TRUE(model.inherit_task_metadata(task_id, stamp));
    }
    ASSERT_TRUE(node.work.strategy().seen.empty());
    ASSERT_EQ(KernelTestProbe::work(node.work.kernel()).tasks.size(), AYNI_TASK_CAPACITY_MAX);
    ASSERT_EQ(KernelTestProbe::work(node.work.kernel()).grants.size(), AYNI_GRANT_CAPACITY_MAX);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(ayni::kernel::awaits_decision(model, node.work.node().info().durable_seq));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    RecordProperty("native_full_boundary_readiness_us", std::to_string(elapsed));
    EXPECT_TRUE(node.work.strategy().seen.empty());
}

// ADR-0038 resource gate: the last retained Goal slot can own multiple
// runnable Tasks at the public Goal-count bound, with the minimal Scope budget.
TEST(WorkAuthorityRestart, maximum_goal_count_keeps_the_last_goals_task_cohort_runnable) {
    AuthorityWork node;
    auto &config = node.work.node().config();
    config.goal_capacity = AYNI_GOAL_CAPACITY_MAX;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MIN;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_MIN;
    config.goal_requirement_capacity = 1;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MIN;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    node.run();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    ASSERT_EQ(node.grant(0, 100, 0, node.work.instance(agent_b)).status, AYNI_OK);
    node.work.node().recover();
    uint64_t goal_id = 0;
    for (uint32_t at = 0; at < AYNI_GOAL_CAPACITY_MAX; ++at) {
        const auto created = node.create(at + 1U == AYNI_GOAL_CAPACITY_MAX ? AYNI_TRIGGER_INTERVAL
                                                                           : AYNI_TRIGGER_NONE);
        ASSERT_EQ(created.status, AYNI_OK);
        goal_id = created.result.object.id;
        node.work.node().recover();
    }
    node.work.node().step(1);
    const auto first = node.work.goal(goal_id).last_task_id;
    ASSERT_NE(first, 0U);
    node.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    node.work.node().step(1);
    const auto second = node.work.goal(goal_id).last_task_id;
    ASSERT_NE(second, first);
    ASSERT_TRUE(node.work.strategy().seen.empty());
    node.work.node().recover();
    // An unrelated missing subject installs a genuine boot hold while all
    // selected references remain valid. The final Goal slot stays executable.
    node.agent_b_available = false;
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    node.work.node().step(1);
    ASSERT_EQ(node.work.strategy().seen.size(), 1U);
    EXPECT_EQ(node.work.strategy().seen.front().snapshot.task_id, first);
    node.work.node().recover();
    node.work.node().step(1);
    ASSERT_EQ(node.work.strategy().seen.size(), 2U);
    EXPECT_EQ(node.work.strategy().seen.back().snapshot.task_id, second);
}

// C-AUTH-020 / ADR-0038: a zero-needs child still depends on its parent's
// retained Grant compatibility. The child's empty own references cannot hide
// an ancestor hold or turn that temporary hold into persistent authorization.
TEST(WorkAuthorityRestart, zero_requirement_child_preserves_ancestor_catalog_compatibility) {
    AuthorityWork source;
    source.run();
    ASSERT_EQ(source.grant().status, AYNI_OK);
    source.work.node().recover();
    const auto parent = source.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(parent.status, AYNI_OK);
    const auto child = source.create(AYNI_TRIGGER_ONCE, parent.result.object.id, "zero");
    ASSERT_EQ(child.status, AYNI_OK);
    source.work.node().step(1);
    source.work.node().recover();
    const auto child_id = child.result.object.id;
    const auto task_id = source.work.goal(child_id).last_task_id;
    ASSERT_NE(task_id, 0U);
    AuthorityWork incompatible;
    incompatible.domain_max = 200;
    incompatible.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    incompatible.run();
    ASSERT_EQ(incompatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    incompatible.advance_work();
    EXPECT_TRUE(incompatible.work.strategy().seen.empty());
    EXPECT_EQ(incompatible.authority(child_id).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
    EXPECT_EQ(incompatible.work.task(task_id).hold_reason, AYNI_REASON_GRANT_CATALOG_CHANGED);
    EXPECT_EQ(incompatible.work.node().step(1).more_ready, 0U);
    AuthorityWork compatible;
    compatible.work.node().sim().durable_storage() =
        incompatible.work.node().sim().durable_storage();
    compatible.run();
    ASSERT_EQ(compatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    compatible.advance_work();
    ASSERT_EQ(compatible.work.strategy().seen.size(), 1U);
    EXPECT_EQ(compatible.work.strategy().seen.front().snapshot.task_id, task_id);
}

// C-AUTH-020 / ADR-0038: multiple Tasks of adjacent Goals keep independent
// static-compatibility results. A table with exactly one held Grant also has
// a held final row; no consumer may convert that hold into a persistent deny.
TEST(WorkAuthorityRestart, shared_tasks_and_adjacent_goals_keep_independent_catalog_holds) {
    AuthorityWork source;
    source.run();
    ASSERT_EQ(source.grant().status, AYNI_OK);
    source.work.node().recover();
    const auto first = source.create(AYNI_TRIGGER_INTERVAL);
    const auto second = source.create(AYNI_TRIGGER_INTERVAL);
    ASSERT_EQ(first.status, AYNI_OK);
    ASSERT_EQ(second.status, AYNI_OK);
    std::vector<uint64_t> tasks;
    for (unsigned period = 0; period < 2; ++period) {
        if (period != 0U) {
            source.work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
        }
        source.work.node().step(1); // Derive the first Goal, before policy work.
        tasks.push_back(source.work.goal(first.result.object.id).last_task_id);
        source.work.node().recover();
        source.work.node().step(1);
        tasks.push_back(source.work.goal(second.result.object.id).last_task_id);
        source.work.node().recover();
    }
    ASSERT_TRUE(source.work.strategy().seen.empty());
    ASSERT_EQ(std::set<uint64_t>(tasks.begin(), tasks.end()).size(), tasks.size());
    for (const auto id : tasks) {
        ASSERT_NE(id, 0U);
    }
    AuthorityWork incompatible;
    incompatible.domain_max = 200;
    incompatible.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    incompatible.run();
    ASSERT_EQ(incompatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    incompatible.advance_work();
    EXPECT_TRUE(incompatible.work.strategy().seen.empty());
    for (const auto goal : {first.result.object.id, second.result.object.id}) {
        EXPECT_EQ(incompatible.authority(goal).phase, AYNI_GOAL_AUTHORITY_AUTHORIZED);
    }
    for (const auto id : tasks) {
        EXPECT_EQ(incompatible.work.task(id).lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(incompatible.work.task(id).hold_reason, AYNI_REASON_GRANT_CATALOG_CHANGED);
    }
    EXPECT_EQ(incompatible.work.node().step(1).more_ready, 0U);
    AuthorityWork compatible;
    compatible.work.node().sim().durable_storage() =
        incompatible.work.node().sim().durable_storage();
    compatible.run();
    ASSERT_EQ(compatible.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    compatible.advance_work();
    std::set<uint64_t> continued;
    for (const auto &seen : compatible.work.strategy().seen) {
        continued.insert(seen.snapshot.task_id);
    }
    EXPECT_EQ(continued, (std::set<uint64_t>(tasks.begin(), tasks.end())));
}

// C-VER-003/C-AUTH-029/033: every copied authority query refuses bad framing,
// unavailable lifecycle and unknown identities without publishing a prefix.
template <typename Info, typename Query>
void query_refusals_preserve_output(ayni_kernel_t *configuring, ayni_kernel_t *running,
                                    Query query) {
    Info output;
    std::memset(&output, 0xa5, sizeof(output));
    output.struct_size = sizeof(output);
    const auto expect_unchanged = [&](ayni_status_t expected, auto call) {
        std::array<uint8_t, sizeof(Info)> before{};
        std::memcpy(before.data(), &output, sizeof(output));
        EXPECT_EQ(call(), expected);
        EXPECT_EQ(std::memcmp(before.data(), &output, sizeof(output)), 0);
    };
    expect_unchanged(AYNI_INVALID_ARGUMENT,
                     [&] { return query(static_cast<ayni_kernel_t *>(nullptr), &output); });
    EXPECT_EQ(query(running, static_cast<Info *>(nullptr)), AYNI_INVALID_ARGUMENT);
    output.struct_size = sizeof(output) - 1U;
    expect_unchanged(AYNI_INVALID_ARGUMENT, [&] { return query(running, &output); });
    output.struct_size = sizeof(output) + 1U;
    expect_unchanged(AYNI_UNSUPPORTED_VERSION, [&] { return query(running, &output); });
    output.struct_size = sizeof(output);
    expect_unchanged(AYNI_INVALID_STATE, [&] { return query(configuring, &output); });
    expect_unchanged(AYNI_NOT_FOUND, [&] { return query(running, &output); });
}

TEST(AuthorityQueries, all_copied_queries_preserve_output_on_each_framing_failure) {
    AuthorityWork configuring;
    configuring.work.node().create();
    AuthorityWork running;
    running.run();
    auto *config = configuring.work.kernel();
    auto *kernel = running.work.kernel();
    query_refusals_preserve_output<ayni_grant_field_info_v1>(
        config, kernel, [](auto *k, auto *out) { return ayni_get_grant_field(k, 0, 0, out); });
    query_refusals_preserve_output<ayni_goal_authority_info_v1>(
        config, kernel, [](auto *k, auto *out) { return ayni_get_goal_authority(k, 0, out); });
    query_refusals_preserve_output<ayni_goal_requirement_info_v1>(
        config, kernel, [](auto *k, auto *out) { return ayni_get_goal_requirement(k, 0, 0, out); });
    query_refusals_preserve_output<ayni_goal_requirement_field_info_v1>(
        config, kernel,
        [](auto *k, auto *out) { return ayni_get_goal_requirement_field(k, 0, 0, 0, out); });
    query_refusals_preserve_output<ayni_task_authority_info_v1>(
        config, kernel, [](auto *k, auto *out) { return ayni_get_task_authority(k, 0, out); });
    query_refusals_preserve_output<ayni_task_authority_reference_v1>(
        config, kernel,
        [](auto *k, auto *out) { return ayni_get_task_authority_reference(k, 0, 0, out); });
}

// C-AUTH-029: invalid output pointers cannot change immutable parameter bytes
// or publish a size; this also covers a genuinely empty parameter payload.
TEST(AuthorityQueries, parameter_query_refuses_null_outputs_and_accepts_empty_payload) {
    AuthorityWork node;
    node.run();
    node.count = 0;
    const auto goal = node.create(AYNI_TRIGGER_NONE);
    ASSERT_EQ(goal.status, AYNI_OK);
    auto *kernel = node.work.kernel();
    uint32_t size = 123;
    EXPECT_EQ(ayni_get_goal_parameters(nullptr, goal.result.object.id, nullptr, 0, &size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 123U);
    EXPECT_EQ(ayni_get_goal_parameters(kernel, goal.result.object.id, nullptr, 1, &size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 123U);
    EXPECT_EQ(ayni_get_goal_parameters(kernel, goal.result.object.id, nullptr, 0, nullptr),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_goal_parameters(kernel, goal.result.object.id, nullptr, 0, &size), AYNI_OK);
    EXPECT_EQ(size, 0U);
}

// ADR-0061: normal revoke/reauthorize/orderly reopen preserves the skipped civil frontier.
TEST(WorkAuthority, calendar_reauthorization_waits_for_time_then_skips_backlog_and_reopens) {
    AuthorityWork node;
    node.trigger_kinds |= AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_DAILY);
    node.run();
    constexpr int64_t day_s = 86400;
    constexpr int64_t first = 1793059200; // 2026-10-27 00:00 UTC.
    node.work.node().sim().set_utc(first);
    const auto grant = node.grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    node.work.node().recover();
    ayni_calendar_rule_v1 calendar{};
    calendar.struct_size = sizeof(calendar);
    calendar.kind = AYNI_CALENDAR_DAILY;
    calendar.rule_version = 1;
    calendar.first_local_s = first;
    calendar.timezone_size = 4;
    std::memcpy(calendar.timezone, "UTC0", calendar.timezone_size);
    auto body = WorkNode::create_body(node.work.instance(agent_a), 0, "authorized", 1, {},
                                      AYNI_TRIGGER_CALENDAR_DAILY);
    body.calendar = &calendar;
    const auto created = node.work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    const auto id = created.result.object.id;
    node.advance_work();
    ASSERT_EQ(node.work.goal(id).occurrences, 1U);
    const auto first_task = node.work.goal(id).last_task_id;
    node.revoke(grant.result.object.id);
    node.advance_work();
    ASSERT_EQ(node.grant().status, AYNI_OK);
    node.work.node().recover();
    ReauthorizationRequest request(node.work.kernel(), id);
    auto intent =
        node.work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = node.command++;
    node.work.node().sim().set_utc(first + 3 * day_s, AYNI_UTC_UNTRUSTED);
    const auto waiting = node.work.submit(intent);
    EXPECT_EQ(waiting.status, AYNI_NOT_READY);
    EXPECT_EQ(waiting.error.reason, AYNI_REASON_TIME_UNTRUSTED);
    EXPECT_EQ(node.work.goal(id).occurrences, 1U);
    node.work.node().sim().set_utc(first + 3 * day_s);
    ASSERT_EQ(node.work.submit(intent).status, AYNI_OK);
    node.advance_work();
    EXPECT_EQ(node.work.goal(id).occurrences, 4U);
    EXPECT_EQ(node.work.goal(id).occurrences_skipped, 3U);
    EXPECT_EQ(node.work.goal(id).last_task_id, first_task);
    node.work.node().reboot();
    node.work.node().recover();
    ASSERT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    node.work.reopen_session();
    node.work.node().sim().set_utc(first + 4 * day_s);
    node.advance_work();
    EXPECT_EQ(node.work.goal(id).occurrences, 5U);
    EXPECT_NE(node.work.goal(id).last_task_id, first_task);
}

} // namespace
