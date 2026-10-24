// ADR-0037: public requirement export, proposals and fixed dependency consumption.
// White-box table/count/stamp assertions below supplement public observations to
// prove refusal leaves no hidden allocation or partial transition behind.
#include "goal_approval_request.h"
#include "kernel/storage/storage_budget.h"
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
            if (allow_parameters) {
                type.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
            }
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
    bool allow_parameters = false;
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
    auto &model = node.work.kernel()->work;
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
    auto &model = node.work.kernel()->work;
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
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 2U);
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
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 2U);
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
    const auto stamp = node.work.kernel()->work.transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 0U);
    EXPECT_EQ(node.work.kernel()->work.transitions.latest().commit_seq, stamp.commit_seq);
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
    const auto before = node.work.kernel()->work.grants.size();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_DENIED);
    EXPECT_EQ(node.work.kernel()->work.grants.size(), before);
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
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 1U);
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
        EXPECT_EQ(node.work.kernel()->work.grants.size(), cut == CutAppend::stored ? 2U : 0U);
        EXPECT_EQ(node.authority(goal.result.object.id).phase, cut == CutAppend::stored
                                                                   ? AYNI_GOAL_AUTHORITY_AUTHORIZED
                                                                   : AYNI_GOAL_AUTHORITY_PROPOSED);
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(node.work.kernel()->work.grants.size(), 2U);
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
                node.work.kernel()->log.idle()) {
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
        EXPECT_EQ(node.work.kernel()->work.grants.size(),
                  fault == StorageFault::unknown_persisted ? 2U : 0U);
        intent.session_id = node.work.session();
        const auto retry = node.work.submit(intent);
        ASSERT_EQ(retry.status, AYNI_OK);
        EXPECT_EQ(node.work.kernel()->work.grants.size(), 2U);
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
    auto &model = node.work.kernel()->work;
    const auto domain = model.capabilities.domain_view(0);
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
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 1U);
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
    auto &model = node.work.kernel()->work;
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
        auto &model = node.work.kernel()->work;
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
    EXPECT_EQ(node.work.kernel()->work.grants.size(), 1U);
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
            if (kernel.maintenance.active()) {
                frozen = true;
                break;
            }
            if (kernel.log.idle() && kernel.completions.in_flight() == 0U) {
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
            if (kernel.maintenance.active()) {
                frozen = true;
                break;
            }
            if (kernel.log.idle() && kernel.completions.in_flight() == 0U) {
                break;
            }
        }
    }
    ASSERT_TRUE(frozen);
    const auto before = kernel.work.transitions.latest();
    EXPECT_EQ(node.work.submit(intent).status, AYNI_BUSY);
    EXPECT_EQ(kernel.work.transitions.latest().commit_seq, before.commit_seq);
    EXPECT_EQ(kernel.work.grants.size(), 0U);
    EXPECT_EQ(node.work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
    for (unsigned step = 0; step < 10000 && !kernel.maintenance.done(); ++step) {
        tick();
    }
    ASSERT_TRUE(kernel.maintenance.done());
    const auto accepted = node.work.submit(intent);
    ASSERT_EQ(accepted.status, AYNI_OK);
    node.work.node().recover();
    EXPECT_EQ(node.work.submit(intent).result.commit_seq, accepted.result.commit_seq);
    EXPECT_EQ(kernel.work.grants.size(), 1U);
    node.work.node().reboot();
    node.work.node().recover();
    EXPECT_EQ(node.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.requirement(goal.result.object.id, 0).grant_id,
              node.work.kernel()->work.grants.at(0).grant_id);
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
    auto &work = node.work.kernel()->work;
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

} // namespace
