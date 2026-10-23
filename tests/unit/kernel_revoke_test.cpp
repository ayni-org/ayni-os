// MA-006 D: C-ABI revocation, volatile deny gates and phased receipts.
// Grant diagnostics are a simulated authorization consumer; no Provider or
// physical actuator is exercised or claimed here.
#include "work_node.h"

#include <ayni/authority.h>
#include <ayni/host_authority.h>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace ayni::test;
using ayni::hosts::sim::CutAppend;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StorageImage;
using ayni::hosts::sim::StoragePort;

class RevokeNode {
    uint64_t principal_;

  public:
    explicit RevokeNode(uint64_t principal = owner_id) : principal_(principal) {
        node.config().grant_capacity = 8;
        node.config().command_capacity = 16;
        node.config().session_capacity = 4;
        node.on_create([this](ayni_kernel_t *kernel) {
            const auto owner = owner_desc(principal_);
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            const auto goal =
                goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE));
            ASSERT_EQ(ayni_register_goal_type(kernel, &goal), AYNI_OK);
            const ayni_capability_desc_v1 capability{
                sizeof(capability), 1, text("test.empty"), 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &capability), AYNI_OK);
        });
    }
    ~RevokeNode() { node.sim().set_storage_capacity(UINT64_MAX); }
    void run() {
        node.run();
        reopen();
        ASSERT_NE(session, 0U);
    }
    uint64_t open(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT) {
        const auto descriptor = session_desc(channel, principal_);
        uint64_t id = 0;
        EXPECT_EQ(ayni_open_session(node.kernel(), &descriptor, &id), AYNI_OK);
        return id;
    }
    void reopen() { session = open(); }
    uint64_t subject() {
        ayni_agent_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_agent(node.kernel(), agent_a, &info), AYNI_OK);
        return info.instance_id;
    }
    Submitted submit(ayni_intent_kind_t kind, const void *body, uint64_t command,
                     uint32_t expected = 0, uint64_t through_session = 0) {
        const ayni_intent_v1 intent{
            sizeof(intent), kind, command, through_session == 0U ? session : through_session,
            expected,       0,    body};
        Submitted out{};
        out.result.struct_size = sizeof(out.result);
        out.error.struct_size = sizeof(out.error);
        out.status = ayni_submit_intent(node.kernel(), &intent, &out.result, &out.error);
        EXPECT_EQ(out.error.status, out.status);
        return out;
    }
    ayni_grant_create_v1 grant_body(uint64_t parent = 0, uint64_t duration = 0,
                                    bool delegation = true) {
        ayni_grant_create_v1 body{};
        body.struct_size = sizeof(body);
        body.capability_version = 1;
        body.capability_id = text("test.empty");
        body.subject_instance_id = subject();
        body.parent_grant_id = parent;
        body.lifetime = duration == 0U ? AYNI_GRANT_PERMANENT : AYNI_GRANT_CURRENT_BOOT;
        body.flags = delegation ? AYNI_GRANT_ALLOW_DELEGATION : 0U;
        body.duration_us = duration;
        body.scope.struct_size = sizeof(body.scope);
        return body;
    }
    Submitted create(uint64_t command, uint64_t parent = 0, uint64_t duration = 0,
                     bool delegation = true) {
        const auto body = grant_body(parent, duration, delegation);
        return submit(AYNI_INTENT_GRANT_CREATE, &body, command, parent == 0U ? 0U : 1U);
    }
    uint64_t durable_grant(uint64_t command = 1, uint64_t parent = 0, uint64_t duration = 0,
                           bool delegation = true) {
        const auto created = create(command, parent, duration, delegation);
        EXPECT_EQ(created.status, AYNI_OK);
        if (created.status != AYNI_OK) {
            return 0;
        }
        store(created.result.commit_seq);
        return created.result.object.id;
    }
    Submitted revoke(uint64_t target, uint64_t command, uint32_t expected = 1,
                     uint64_t through_session = 0) {
        const ayni_grant_revoke_v1 body{sizeof(body), 0, target};
        return submit(AYNI_INTENT_GRANT_REVOKE, &body, command, expected, through_session);
    }
    ayni_goal_create_v1 goal_body() {
        return WorkNode::create_body(subject(), 0, "folder", 1, {nullptr, 0}, AYNI_TRIGGER_NONE);
    }
    Submitted create_goal(uint64_t command) {
        const auto body = goal_body();
        return submit(AYNI_INTENT_GOAL_CREATE, &body, command);
    }
    Submitted cancel_goal(uint64_t id, uint64_t command) {
        const ayni_goal_target_v1 target{sizeof(target), 0, id};
        return submit(AYNI_INTENT_GOAL_CANCEL, &target, command, 1);
    }
    ayni_grant_info_v1 grant(uint64_t id) {
        ayni_grant_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_grant(node.kernel(), id, &out), AYNI_OK);
        return out;
    }
    ayni_intent_receipt_v1 receipt(uint64_t command) {
        ayni_intent_receipt_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_intent_receipt(node.kernel(), principal_, command, &out), AYNI_OK);
        return out;
    }
    ayni_step_report_v1 tick() {
        const auto report = node.step(1);
        const auto completed = node.sim().complete_storage();
        if (completed == 0U && report.more_ready == 0U && node.sim().pending_storage() == 0U &&
            report.next_deadline_us != UINT64_MAX &&
            report.next_deadline_us > node.sim().now_us()) {
            node.sim().advance_us(report.next_deadline_us - node.sim().now_us());
        }
        return report;
    }
    void store(uint64_t sequence) {
        for (unsigned round = 0; round < 5000 && node.info().durable_seq < sequence; ++round) {
            tick();
        }
        ASSERT_GE(node.info().durable_seq, sequence);
    }
    void settle() {
        for (unsigned round = 0; round < 5000; ++round) {
            tick();
            if (node.lifecycle() == AYNI_LIFECYCLE_RESTRICTED) {
                return;
            }
            // White-box: quiescence only; all semantic assertions use ABI
            // receipts/GrantInfo rather than these scheduler internals.
            if (node.lifecycle() == AYNI_LIFECYCLE_RUNNING && node.kernel()->log.idle() &&
                node.kernel()->completions.in_flight() == 0U &&
                !node.kernel()->maintenance.active() &&
                node.kernel()->work.commands.pending_size() == 0U) {
                return;
            }
        }
        FAIL() << "revocation storage never settled";
    }
    void fail_three_appends() {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            node.sim().inject_fault(StoragePort::append, StorageFault::fail);
        }
    }
    void pending_is(uint64_t command, uint64_t target) {
        const auto out = receipt(command);
        EXPECT_EQ(out.phase, AYNI_INTENT_PENDING);
        EXPECT_EQ(out.effect, AYNI_INTENT_EFFECT_NONE);
        EXPECT_EQ(out.commit_seq, 0U);
        EXPECT_EQ(out.applied_boot_epoch, 0U);
        EXPECT_EQ(out.object.kind, AYNI_OBJECT_GRANT);
        EXPECT_EQ(out.object.id, target);
        EXPECT_EQ(out.object.revision, 0U);
    }
    bool wait_for_snapshot(uint64_t &next_command) {
        for (unsigned cycle = 0; cycle < 1000; ++cycle) {
            const auto created = create_goal(next_command++);
            EXPECT_EQ(created.status, AYNI_OK);
            if (created.status != AYNI_OK) {
                return false;
            }
            for (unsigned phase = 0; phase < 2; ++phase) {
                for (unsigned round = 0; round < 1000; ++round) {
                    tick();
                    if (node.kernel()->maintenance.active()) {
                        return true;
                    }
                    if (node.kernel()->log.idle() && node.kernel()->completions.in_flight() == 0U) {
                        break;
                    }
                }
                if (phase == 0U) {
                    EXPECT_EQ(cancel_goal(created.result.object.id, next_command++).status,
                              AYNI_OK);
                }
            }
        }
        return false;
    }

    Node node;
    uint64_t session = 0;
};

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, pending_without_storage_space_immediately_denies_every_ancestor_link) {
    RevokeNode work;
    work.run();
    const uint64_t root = work.durable_grant(1);
    const uint64_t child = work.durable_grant(2, root);
    const uint64_t leaf = work.durable_grant(3, child);
    const auto before = work.grant(root);
    work.node.sim().set_storage_capacity(0);
    const auto accepted = work.revoke(root, 4);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(accepted.result.phase, AYNI_INTENT_PENDING);
    EXPECT_EQ(accepted.result.effect, AYNI_INTENT_EFFECT_NONE);
    EXPECT_EQ(accepted.result.commit_seq, 0U);
    EXPECT_EQ(accepted.result.applied_boot_epoch, 0U);
    EXPECT_EQ(accepted.result.object.revision, 0U);
    work.pending_is(4, root);
    for (const uint64_t id : {root, child, leaf}) {
        EXPECT_EQ(work.grant(id).effective_reason, AYNI_REASON_GRANT_REVOKED) << id;
    }
    EXPECT_EQ(work.grant(root).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(work.grant(root).commit_seq, before.commit_seq);
    EXPECT_EQ(work.grant(root).revision, before.revision);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.settle();
    EXPECT_EQ(work.receipt(4).phase, AYNI_INTENT_DURABLE);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, actual_journal_backlog_still_accepts_a_pending_deny) {
    RevokeNode work;
    work.node.config().storage_buffer_bytes = smallest_storage_buffer(work.node.config());
    work.run();
    const uint64_t target = work.durable_grant();
    bool full = false;
    uint64_t command = 10;
    for (unsigned round = 0; round < 1000 && !full; ++round) {
        const auto created = work.create_goal(command++);
        if (created.status == AYNI_BUSY) {
            full = true;
            break;
        }
        ASSERT_EQ(created.status, AYNI_OK);
        const auto cancelled = work.cancel_goal(created.result.object.id, command++);
        if (cancelled.status == AYNI_BUSY) {
            full = true;
        } else {
            ASSERT_EQ(cancelled.status, AYNI_OK);
        }
    }
    ASSERT_TRUE(full);
    const auto revoked = work.revoke(target, command++);
    ASSERT_EQ(revoked.status, AYNI_OK);
    EXPECT_EQ(revoked.result.phase, AYNI_INTENT_PENDING);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_GRANT_REVOKED);
    work.settle();
    EXPECT_EQ(work.receipt(command - 1U).phase, AYNI_INTENT_DURABLE);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, applied_and_durable_receipts_keep_original_result_across_a_new_session) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    const auto revoked = work.revoke(target, 2);
    ASSERT_EQ(revoked.status, AYNI_OK);
    EXPECT_EQ(revoked.result.phase, AYNI_INTENT_APPLIED);
    EXPECT_EQ(revoked.result.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_GT(revoked.result.commit_seq, work.node.info().durable_seq);
    EXPECT_EQ(revoked.result.applied_boot_epoch, work.node.info().boot_epoch);
    EXPECT_EQ(revoked.result.object.revision, 2U);
    EXPECT_EQ(work.receipt(2).phase, AYNI_INTENT_APPLIED);
    const uint64_t original = revoked.result.commit_seq;
    ASSERT_EQ(ayni_close_session(work.node.kernel(), work.session), AYNI_OK);
    work.reopen();
    const auto retried = work.revoke(target, 2);
    ASSERT_EQ(retried.status, AYNI_OK);
    EXPECT_EQ(retried.result.commit_seq, original);
    work.settle();
    const auto durable = work.receipt(2);
    EXPECT_EQ(durable.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(durable.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(durable.commit_seq, original);
    EXPECT_EQ(durable.applied_boot_epoch, 0U);
    const auto after = work.revoke(target, 2);
    ASSERT_EQ(after.status, AYNI_OK);
    EXPECT_EQ(after.result.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(after.result.commit_seq, original);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, ordinary_commands_use_the_same_applied_and_durable_receipt_phases) {
    RevokeNode work;
    work.run();
    const auto created = work.create_goal(1);
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(created.result.phase, AYNI_INTENT_APPLIED);
    EXPECT_EQ(created.result.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(work.receipt(1).object.kind, AYNI_OBJECT_GOAL);
    work.settle();
    const auto durable = work.receipt(1);
    EXPECT_EQ(durable.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(durable.commit_seq, created.result.commit_seq);
    EXPECT_EQ(durable.applied_boot_epoch, 0U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, pending_identity_precedes_every_implemented_kind_and_later_window_floor) {
    RevokeNode work;
    work.node.config().command_capacity = 1;
    work.run();
    const uint64_t target = work.durable_grant();
    work.node.sim().set_storage_capacity(0);
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    ASSERT_EQ(work.create_goal(50).status, AYNI_OK);
    ASSERT_EQ(work.create_goal(60).status, AYNI_OK);
    for (ayni_intent_kind_t kind = AYNI_INTENT_GOAL_CREATE; kind <= AYNI_INTENT_GRANT_CREATE;
         ++kind) {
        SCOPED_TRACE(kind);
        const auto goal = work.goal_body();
        const auto create = work.grant_body();
        const ayni_goal_target_v1 goal_target{sizeof(goal_target), 0, 999};
        const ayni_task_target_v1 task_target{sizeof(task_target), 0, 999};
        const ayni_instance_target_v1 instance_target{sizeof(instance_target), 0, work.subject()};
        const void *body = &goal_target;
        uint32_t expected = 1;
        if (kind == AYNI_INTENT_GOAL_CREATE) {
            body = &goal;
            expected = 0;
        } else if (kind == AYNI_INTENT_TASK_CANCEL) {
            body = &task_target;
        } else if (kind >= AYNI_INTENT_INSTANCE_PAUSE && kind <= AYNI_INTENT_INSTANCE_STOP) {
            body = &instance_target;
        } else if (kind == AYNI_INTENT_GRANT_CREATE) {
            body = &create;
            expected = 0;
        }
        const auto conflict = work.submit(kind, body, 2, expected);
        EXPECT_EQ(conflict.status, AYNI_IDEMPOTENCY_CONFLICT);
        EXPECT_EQ(conflict.error.reason, AYNI_REASON_COMMAND_CONFLICT);
    }
    work.pending_is(2, target);
    EXPECT_EQ(work.revoke(target, 2).status, AYNI_OK);
    EXPECT_EQ(work.revoke(target, 2, 2).status, AYNI_IDEMPOTENCY_CONFLICT);
    work.settle();
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, a_second_command_for_one_pending_target_is_busy_and_does_not_bind_its_id) {
    RevokeNode work;
    work.run();
    const uint64_t first = work.durable_grant(1);
    const uint64_t second = work.durable_grant(2);
    work.node.sim().set_storage_capacity(0);
    ASSERT_EQ(work.revoke(first, 3).status, AYNI_OK);
    const auto refused = work.revoke(first, 4);
    EXPECT_EQ(refused.status, AYNI_BUSY);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_REVOCATION_PENDING);
    EXPECT_EQ(work.revoke(second, 4).status, AYNI_OK);
    work.pending_is(3, first);
    work.pending_is(4, second);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.settle();
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, new_revocation_validates_shape_channel_and_current_revision_before_acceptance) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    const uint64_t voice = work.open(AYNI_CHANNEL_VOICE);
    ayni_grant_revoke_v1 malformed{sizeof(ayni_grant_revoke_v1), 1, target};
    EXPECT_EQ(work.submit(AYNI_INTENT_GRANT_REVOKE, &malformed, 2, 1, voice).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(work.revoke(target, 2, 1, voice).status, AYNI_DENIED);
    EXPECT_EQ(work.revoke(target, 0).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(work.revoke(target, 2, 0).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(work.revoke(0, 2).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(work.revoke(UINT64_MAX, 2).status, AYNI_NOT_FOUND);
    EXPECT_EQ(work.revoke(target, 2, 2).status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_NONE);
    ayni_intent_receipt_v1 out{};
    out.struct_size = sizeof(out);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 2, &out), AYNI_NOT_FOUND);
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, receipt_query_errors_leave_caller_owned_output_unchanged) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    work.node.sim().set_storage_capacity(0);
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    ayni_intent_receipt_v1 out{};
    std::memset(&out, 0xA5, sizeof(out));
    out.struct_size = sizeof(out);
    const auto original = out;
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 999, &out), AYNI_NOT_FOUND);
    EXPECT_EQ(std::memcmp(&out, &original, sizeof(out)), 0);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id + 1U, 2, &out), AYNI_DENIED);
    EXPECT_EQ(std::memcmp(&out, &original, sizeof(out)), 0);
    EXPECT_EQ(ayni_get_intent_receipt(nullptr, owner_id, 2, &out), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 0, &out),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 2, nullptr),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&out, &original, sizeof(out)), 0);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.settle();
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, an_expired_target_gets_a_durable_no_change_without_rewriting_terminal_order) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant(1, 0, 1000000);
    const auto active = work.grant(target);
    work.node.sim().advance_us(active.deadline_us - work.node.sim().now_us());
    for (unsigned round = 0; round < 100 && work.grant(target).lifecycle == AYNI_GRANT_ACTIVE;
         ++round) {
        work.node.step(1);
    }
    const auto expired = work.grant(target);
    ASSERT_EQ(expired.lifecycle, AYNI_GRANT_EXPIRED);
    work.store(expired.commit_seq);
    const auto revoked = work.revoke(target, 2, 2);
    ASSERT_EQ(revoked.status, AYNI_OK);
    EXPECT_EQ(revoked.result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    work.settle();
    EXPECT_EQ(work.receipt(2).phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(work.receipt(2).effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(work.grant(target).commit_seq, expired.commit_seq);
}

// C-AUTH-026 / ADR-0031: bounded cleanup and retained recovery preserve terminal facts.
TEST(RevokeApi, a_new_command_for_a_durably_revoked_target_preserves_cleanup_and_terminal_stamps) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    work.settle();
    for (unsigned round = 0; round < 100; ++round) {
        const auto info = work.grant(target);
        if (info.cleanup_after_grant_id == info.cleanup_through_grant_id) {
            work.store(info.cleanup_commit_seq);
            break;
        }
        work.tick();
    }
    const auto before = work.grant(target);
    const auto no_change = work.revoke(target, 3, 2);
    ASSERT_EQ(no_change.status, AYNI_OK);
    EXPECT_EQ(no_change.result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    work.settle();
    const auto after = work.grant(target);
    EXPECT_EQ(after.commit_seq, before.commit_seq);
    EXPECT_EQ(after.cleanup_after_grant_id, before.cleanup_after_grant_id);
    EXPECT_EQ(after.cleanup_through_grant_id, before.cleanup_through_grant_id);
    EXPECT_EQ(after.cleanup_commit_seq, before.cleanup_commit_seq);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, process_restart_does_not_claim_to_preserve_an_unpersisted_pending_control) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    work.node.sim().set_storage_capacity(0);
    ASSERT_EQ(work.revoke(target, 2).result.phase, AYNI_INTENT_PENDING);
    work.pending_is(2, target);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.node.power_cut_and_start(CutAppend::lost);
    work.node.recover();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen();
    EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_NONE);
    ayni_intent_receipt_v1 missing{};
    missing.struct_size = sizeof(missing);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 2, &missing), AYNI_NOT_FOUND);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, all_pinned_results_block_ordinary_work_but_keep_the_next_revoke_pending) {
    RevokeNode work;
    work.node.config().command_capacity = 1;
    work.run();
    const uint64_t first = work.durable_grant(1);
    const uint64_t second = work.durable_grant(2);
    const auto applied = work.revoke(first, 3);
    ASSERT_EQ(applied.status, AYNI_OK);
    ASSERT_EQ(applied.result.phase, AYNI_INTENT_APPLIED);
    const auto pending = work.revoke(second, 4);
    ASSERT_EQ(pending.status, AYNI_OK);
    EXPECT_EQ(pending.result.phase, AYNI_INTENT_PENDING);
    EXPECT_EQ(work.receipt(3).phase, AYNI_INTENT_APPLIED);
    work.pending_is(4, second);
    EXPECT_EQ(work.grant(second).effective_reason, AYNI_REASON_GRANT_REVOKED);
    const auto refused = work.create_goal(5);
    EXPECT_EQ(refused.status, AYNI_BUSY);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_COMMAND_PINNED);
    work.settle();
    EXPECT_EQ(work.receipt(4).phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(work.create_goal(5).status, AYNI_OK);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, durable_retry_does_not_publish_another_pending_revoke_or_evict_its_own_result) {
    RevokeNode work;
    work.node.config().command_capacity = 1;
    work.run();
    const uint64_t first = work.durable_grant(1);
    const uint64_t second = work.durable_grant(2);
    const auto first_revoke = work.revoke(first, 3);
    ASSERT_EQ(first_revoke.status, AYNI_OK);
    ASSERT_EQ(first_revoke.result.phase, AYNI_INTENT_APPLIED);
    const auto waiting = work.revoke(second, 4);
    ASSERT_EQ(waiting.status, AYNI_OK);
    ASSERT_EQ(waiting.result.phase, AYNI_INTENT_PENDING);
    // A one-item step consumes A's completion and unpins its result, leaving
    // no item budget to publish B. No private queue/window mutation is used.
    for (unsigned round = 0; round < 1000 && work.receipt(3).phase != AYNI_INTENT_DURABLE;
         ++round) {
        work.tick();
    }
    const auto durable = work.receipt(3);
    ASSERT_EQ(durable.phase, AYNI_INTENT_DURABLE);
    ASSERT_EQ(durable.commit_seq, first_revoke.result.commit_seq);
    work.pending_is(4, second);
    const auto before = work.grant(second);
    ASSERT_EQ(before.lifecycle, AYNI_GRANT_ACTIVE);
    const auto retry = work.revoke(first, 3);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.object.id, first);
    EXPECT_EQ(retry.result.commit_seq, durable.commit_seq);
    EXPECT_EQ(retry.result.effect, durable.effect);
    EXPECT_EQ(retry.result.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(retry.result.applied_boot_epoch, 0U);
    work.pending_is(4, second);
    const auto after = work.grant(second);
    EXPECT_EQ(after.lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.commit_seq, before.commit_seq);
    EXPECT_EQ(work.receipt(3).commit_seq, durable.commit_seq);
    work.settle();
    EXPECT_EQ(work.receipt(4).phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(work.grant(second).lifecycle, AYNI_GRANT_REVOKED);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, unknown_persisted_result_reconciles_original_canonical_and_original_sequence) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    const auto old_boot = work.node.info().boot_epoch;
    work.node.sim().inject_fault(StoragePort::append, StorageFault::unknown_persisted);
    work.node.sim().inject_fault(StoragePort::append, StorageFault::fail);
    work.node.sim().inject_fault(StoragePort::append, StorageFault::fail);
    const auto revoked = work.revoke(target, 2);
    ASSERT_EQ(revoked.status, AYNI_OK);
    const uint64_t original = revoked.result.commit_seq;
    ASSERT_GT(original, 0U);
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_GT(work.node.info().boot_epoch, old_boot);
    const auto receipt = work.receipt(2);
    EXPECT_EQ(receipt.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(receipt.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(receipt.commit_seq, original);
    EXPECT_EQ(receipt.applied_boot_epoch, 0U);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_GRANT_REVOKED);
    work.reopen();
    const auto retry = work.revoke(target, 2);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, original);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, lost_unpersisted_creation_gets_target_gone_without_reusing_old_queued_durability) {
    RevokeNode work;
    work.run();
    const auto old_boot = work.node.info().boot_epoch;
    work.fail_three_appends();
    const auto created = work.create(1); // Neither this fact nor its mapping reaches storage.
    ASSERT_EQ(created.status, AYNI_OK);
    const auto revoked = work.revoke(created.result.object.id, 2);
    ASSERT_EQ(revoked.status, AYNI_OK);
    ASSERT_EQ(revoked.result.phase, AYNI_INTENT_APPLIED);
    const uint64_t abandoned = revoked.result.commit_seq;
    bool reloaded = false;
    bool durable = false;
    for (unsigned round = 0; round < 5000 && !durable; ++round) {
        work.tick();
        const auto info = work.node.info();
        const auto receipt = work.receipt(2); // Valid even while the old Session is gone.
        if (info.boot_epoch > old_boot) {
            reloaded = true;
            if (receipt.phase == AYNI_INTENT_APPLIED) {
                EXPECT_EQ(receipt.applied_boot_epoch, info.boot_epoch);
            }
            if (receipt.phase == AYNI_INTENT_DURABLE) {
                EXPECT_EQ(receipt.effect, AYNI_INTENT_EFFECT_TARGET_GONE);
                EXPECT_NE(receipt.commit_seq, abandoned);
                EXPECT_EQ(receipt.object.id, created.result.object.id);
                EXPECT_EQ(receipt.object.revision, 0U);
                durable = true;
            }
        }
    }
    ASSERT_TRUE(reloaded);
    ASSERT_TRUE(durable);
    ayni_grant_info_v1 missing{};
    missing.struct_size = sizeof(missing);
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), created.result.object.id, &missing),
              AYNI_NOT_FOUND);
    EXPECT_EQ(ayni_close_session(work.node.kernel(), work.session), AYNI_NOT_FOUND);
    work.reopen();
    const auto repeated = work.revoke(created.result.object.id, 2);
    ASSERT_EQ(repeated.status, AYNI_OK);
    EXPECT_EQ(repeated.result.effect, AYNI_INTENT_EFFECT_TARGET_GONE);
    EXPECT_EQ(repeated.result.phase, AYNI_INTENT_DURABLE);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, failed_writer_isolation_preserves_the_gate_and_reports_no_durable_result) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    const uint64_t boot = work.node.info().boot_epoch;
    work.fail_three_appends();
    work.node.sim().inject_fault(StoragePort::fence, StorageFault::unavailable);
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.receipt(2).applied_boot_epoch, boot);
    EXPECT_NE(work.receipt(2).phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_GRANT_REVOKED);
    // White-box: independent control state must survive this failed handoff.
    EXPECT_TRUE(work.node.kernel()->work.grants.gated(target));
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi,
     a_reload_read_failure_keeps_the_accepted_control_after_restored_facts_are_discarded) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    work.fail_three_appends();
    work.node.sim().inject_fault(StoragePort::read_prefix, StorageFault::fail);
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    work.pending_is(2, target);
    ayni_intent_receipt_v1 ordinary{};
    ordinary.struct_size = sizeof(ordinary);
    ordinary.phase = 999;
    const auto before = ordinary;
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 1, &ordinary), AYNI_NOT_READY);
    EXPECT_EQ(std::memcmp(&ordinary, &before, sizeof(ordinary)), 0);
    // White-box: unavailable restored data cannot make its volatile gate disappear.
    EXPECT_TRUE(work.node.kernel()->work.grants.gated(target));
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, shutdown_after_reload_read_failure_keeps_unapplied_controls_without_a_writer) {
    RevokeNode work;
    work.node.config().storage_buffer_bytes = smallest_storage_buffer(work.node.config());
    work.node.config().storage_log_max_bytes = 65536;
    work.run();
    const uint64_t target = work.durable_grant();
    uint64_t next_command = 100;
    ASSERT_TRUE(work.wait_for_snapshot(next_command));
    const uint64_t command = next_command++;
    const auto accepted = work.revoke(target, command);
    ASSERT_EQ(accepted.status, AYNI_OK);
    ASSERT_EQ(accepted.result.phase, AYNI_INTENT_PENDING);
    // A snapshot failure leaves the CommitLog itself healthy. Thus its room
    // cannot stand in for a usable writer after isolation clears the model.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        work.node.sim().inject_fault(StoragePort::write_snapshot, StorageFault::fail);
    }
    work.node.sim().inject_fault(StoragePort::read_prefix, StorageFault::fail);
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    work.pending_is(command, target);
    // White-box read-only preconditions: actual failed reload has reset the
    // allocator/clock, while the older log object still has physical room.
    ASSERT_EQ(work.node.kernel()->work.ids.boot_epoch(), 0U);
    ASSERT_EQ(work.node.kernel()->work.transitions.next_commit_seq(), 0U);
    ASSERT_TRUE(work.node.kernel()->log.has_room());
    ASSERT_TRUE(work.node.kernel()->work.grants.gated(target));
    ASSERT_EQ(ayni_begin_shutdown(work.node.kernel()), AYNI_OK);
    work.pending_is(command, target);
    for (unsigned round = 0; round < 100 && work.node.lifecycle() != AYNI_LIFECYCLE_STOPPED;
         ++round) {
        work.node.step(1);
        work.node.sim().complete_storage();
        ASSERT_NE(work.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    }
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(work.node.kernel()->work.transitions.next_commit_seq(), 0U);
    const auto *control = work.node.kernel()->work.commands.pending(owner_id, command);
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->queued_boot_epoch, 0U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_TRUE(work.node.kernel()->work.grants.gated(target));
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi,
     binding_mismatch_on_reload_keeps_the_gate_and_host_authenticated_pending_diagnostic) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    StorageImage foreign_image;
    {
        // Build a valid complete source through public API, with the same
        // commit/ID allocation order and a different authenticated owner.
        RevokeNode foreign(owner_id + 1U);
        foreign.run();
        EXPECT_EQ(foreign.durable_grant(), target);
        foreign_image = foreign.node.sim().durable_storage();
    }
    work.fail_three_appends();
    ASSERT_EQ(work.revoke(target, 2).status, AYNI_OK);
    work.node.sim().durable_storage() = foreign_image;
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node.info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
    work.pending_is(2, target);
    EXPECT_TRUE(work.node.kernel()->work.grants.gated(target));
}

// C-AUTH-026 / ADR-0031: bounded cleanup and retained recovery preserve terminal facts.
TEST(RevokeApi, cleanup_is_bounded_and_never_rewrites_the_roots_terminal_commit_order) {
    RevokeNode work;
    work.run();
    const uint64_t root = work.durable_grant(1);
    const std::array<uint64_t, 3> children{work.durable_grant(2, root), work.durable_grant(3, root),
                                           work.durable_grant(4, root)};
    const auto revoked = work.revoke(root, 5);
    ASSERT_EQ(revoked.status, AYNI_OK);
    const uint64_t terminal = work.grant(root).commit_seq;
    auto count = [&] {
        uint32_t result = 0;
        for (const uint64_t child : children) {
            result += work.grant(child).lifecycle == AYNI_GRANT_REVOKED ? 1U : 0U;
        }
        return result;
    };
    uint32_t before = count();
    for (unsigned round = 0; round < 100 && before < children.size(); ++round) {
        work.node.step(1);
        const uint32_t after = count();
        EXPECT_LE(after - before, 1U); // One max_items item can settle at most one descendant.
        EXPECT_EQ(work.grant(root).commit_seq, terminal);
        before = after;
    }
    ASSERT_EQ(before, children.size());
    work.settle();
    for (unsigned round = 0; round < 100; ++round) {
        const auto info = work.grant(root);
        if (info.cleanup_after_grant_id == info.cleanup_through_grant_id) {
            break;
        }
        work.tick();
    }
    const auto cleaned = work.grant(root);
    EXPECT_EQ(cleaned.cleanup_after_grant_id, cleaned.cleanup_through_grant_id);
    EXPECT_GT(cleaned.cleanup_commit_seq, terminal);
    EXPECT_EQ(cleaned.commit_seq, terminal);
    for (const uint64_t child : children) {
        EXPECT_EQ(work.grant(child).revoked_by_root_id, root);
        EXPECT_EQ(work.grant(child).effective_reason, AYNI_REASON_GRANT_REVOKED);
    }
}

// C-AUTH-026 / ADR-0031: bounded cleanup and retained recovery preserve terminal facts.
TEST(RevokeApi, only_a_durable_terminal_leaf_can_make_room_after_revocation_cleanup) {
    RevokeNode work;
    work.node.config().grant_capacity = 2;
    work.run();
    const uint64_t root = work.durable_grant(1);
    const uint64_t child = work.durable_grant(2, root);
    ASSERT_EQ(work.revoke(root, 3).status, AYNI_OK);
    for (unsigned round = 0; round < 100 && work.grant(child).lifecycle != AYNI_GRANT_REVOKED;
         ++round) {
        work.node.step(1); // Deliberately do not complete any cleanup write.
    }
    ASSERT_EQ(work.grant(child).lifecycle, AYNI_GRANT_REVOKED);
    const auto refused = work.create(4);
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_GRANT_CAPACITY);
    work.settle();
    const auto accepted = work.create(4); // A refused command did not bind its identity.
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_NE(accepted.result.object.id, child);
    ayni_grant_info_v1 missing{};
    missing.struct_size = sizeof(missing);
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), child, &missing), AYNI_NOT_FOUND);
    EXPECT_EQ(work.grant(root).lifecycle,
              AYNI_GRANT_REVOKED); // Retained child blocked parent first.
}

// C-AUTH-026 / ADR-0031: bounded cleanup and retained recovery preserve terminal facts.
TEST(RevokeApi, revision_two_accepted_after_uncommitted_cleanup_can_change_restored_active_child) {
    RevokeNode work;
    work.run();
    const uint64_t root = work.durable_grant(1);
    const uint64_t child = work.durable_grant(2, root);
    const auto root_revoke = work.revoke(root, 3);
    ASSERT_EQ(root_revoke.status, AYNI_OK);
    work.store(root_revoke.result.commit_seq); // Parent revoke is permanent, cleanup is not.
    for (unsigned round = 0; round < 100 && work.grant(child).lifecycle != AYNI_GRANT_REVOKED;
         ++round) {
        work.node.step(1);
    }
    ASSERT_EQ(work.grant(child).revision, 2U);
    ASSERT_GT(work.grant(child).commit_seq, work.node.info().durable_seq);
    work.node.sim().set_storage_capacity(0);
    const auto accepted = work.revoke(child, 4, 2);
    ASSERT_EQ(accepted.status, AYNI_OK);
    ASSERT_EQ(accepted.result.phase, AYNI_INTENT_PENDING);
    work.fail_three_appends();
    const uint64_t old_boot = work.node.info().boot_epoch;
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.settle();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_GT(work.node.info().boot_epoch, old_boot);
    const auto receipt = work.receipt(4);
    EXPECT_EQ(receipt.phase, AYNI_INTENT_DURABLE);
    EXPECT_EQ(receipt.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(receipt.object.revision, 2U);
    EXPECT_EQ(work.grant(child).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(work.grant(child).revoked_by_root_id, child);
    work.reopen();
    const auto retry = work.revoke(child, 4, 2);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, receipt.commit_seq);
}

// C-AUTH-026 / ADR-0031: bounded cleanup and retained recovery preserve terminal facts.
TEST(RevokeApi, paged_snapshot_keeps_its_frozen_model_while_accepting_a_volatile_deny) {
    RevokeNode work;
    work.node.config().grant_capacity = 16;
    work.node.config().command_capacity = 32;
    work.node.config().storage_buffer_bytes = smallest_storage_buffer(work.node.config());
    work.node.config().storage_log_max_bytes = 65536;
    work.run();
    uint64_t target = 0;
    for (uint64_t command = 1; command <= 16; ++command) {
        const auto id = work.durable_grant(command);
        ASSERT_NE(id, 0U);
        if (command == 1U) {
            target = id;
        }
    }
    uint64_t next_command = 100;
    ASSERT_TRUE(work.wait_for_snapshot(next_command));
    // White-box: require more than one payload page, not just a header
    // request that happens to share the maintenance state machine.
    ASSERT_GT(work.node.kernel()->maintenance.metadata().payload_length,
              work.node.config().storage_buffer_bytes);
    const auto before = work.grant(target);
    const uint64_t command = next_command++;
    const auto pending = work.revoke(target, command);
    ASSERT_EQ(pending.status, AYNI_OK);
    EXPECT_EQ(pending.result.phase, AYNI_INTENT_PENDING);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_GRANT_REVOKED);
    for (unsigned round = 0; round < 1000 && work.node.kernel()->maintenance.active(); ++round) {
        EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_ACTIVE);
        EXPECT_EQ(work.grant(target).commit_seq, before.commit_seq);
        work.pending_is(command, target);
        work.tick();
    }
    ASSERT_FALSE(work.node.kernel()->maintenance.active());
    work.pending_is(command, target);
    // Crash before the accepted control is applied. The activated snapshot
    // must be valid and retain the frozen ACTIVE fact, excluding the gate.
    work.node.power_cut_and_start(CutAppend::lost);
    work.node.recover();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen();
    EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(work.grant(target).commit_seq, before.commit_seq);
    EXPECT_EQ(work.grant(target).effective_reason, AYNI_REASON_NONE);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(RevokeApi, shutdown_drains_an_already_accepted_control_without_accepting_new_revocations) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    work.node.sim().set_storage_capacity(0);
    ASSERT_EQ(work.revoke(target, 2).result.phase, AYNI_INTENT_PENDING);
    ASSERT_EQ(ayni_begin_shutdown(work.node.kernel()), AYNI_OK);
    EXPECT_EQ(work.revoke(target, 3).status, AYNI_NOT_READY);
    work.pending_is(2, target);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    work.node.drain();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    work.node.reboot();
    work.node.recover();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen();
    EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(work.receipt(2).phase, AYNI_INTENT_DURABLE);
}
// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, ordinary_receipt_floor_and_bad_query_shapes_preserve_the_output) {
    RevokeNode work;
    work.node.config().command_capacity = 1;
    work.run();
    const auto first = work.create_goal(1);
    ASSERT_EQ(first.status, AYNI_OK);
    work.store(first.result.commit_seq);
    const auto later = work.create_goal(2);
    ASSERT_EQ(later.status, AYNI_OK);
    work.store(later.result.commit_seq);
    ayni_intent_receipt_v1 out{};
    out.struct_size = sizeof(out);
    out.phase = 999;
    auto before = out;
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 1, &out), AYNI_EXPIRED);
    EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), 0, 2, &out), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 0, &out),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0);
    out.struct_size = sizeof(out) - 1U;
    before = out;
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 2, &out),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, receipt_window_is_not_exposed_before_initial_recovery_finishes) {
    RevokeNode work;
    work.node.create_and_start();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
    ayni_intent_receipt_v1 out{};
    out.struct_size = sizeof(out);
    out.phase = 999;
    const auto before = out;
    EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 1, &out), AYNI_NOT_READY);
    EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(RevokeApi, a_commandless_submit_reports_a_fresh_applied_stamp_without_a_receipt_mapping) {
    RevokeNode work;
    work.run();
    const auto first = work.create_goal(0);
    ASSERT_EQ(first.status, AYNI_OK);
    EXPECT_EQ(first.result.phase, AYNI_INTENT_APPLIED);
    EXPECT_EQ(first.result.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(first.result.applied_boot_epoch, work.node.info().boot_epoch);
    EXPECT_GT(first.result.commit_seq, work.node.info().durable_seq);
    work.store(first.result.commit_seq);
    const auto second = work.create_goal(0);
    ASSERT_EQ(second.status, AYNI_OK);
    EXPECT_EQ(second.result.phase, AYNI_INTENT_APPLIED);
    EXPECT_GT(second.result.commit_seq, first.result.commit_seq);
    EXPECT_NE(second.result.object.id, first.result.object.id);
}

// C-WIRE-019 / ADR-0031: a restored mapping is private until the complete
// recovered prefix and its new writer have finished, even if lookup can find it.
TEST(RevokeApi, a_mapping_already_restored_during_recovery_is_not_a_public_receipt) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant();
    work.node.reboot();
    for (unsigned round = 0; round < 1000; ++round) {
        // White-box: observe real replay progress; do not fabricate a mapping or lifecycle.
        if (work.node.kernel()->work.commands.size() != 0U) {
            ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
            ayni_intent_receipt_v1 out{};
            out.struct_size = sizeof(out);
            out.phase = 999;
            const auto before = out;
            EXPECT_EQ(ayni_get_intent_receipt(work.node.kernel(), owner_id, 1, &out),
                      AYNI_NOT_READY);
            EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0);
            work.node.recover();
            ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
            EXPECT_EQ(work.receipt(1).object.id, target);
            EXPECT_EQ(work.receipt(1).phase, AYNI_INTENT_DURABLE);
            return;
        }
        work.tick();
    }
    FAIL() << "fixture did not reach a partially restored command window";
}

} // namespace

namespace {
// C-AUTH-026/027 / ADR-0035 decision 11: an accepted revocation consumes
// the available item before another Grant's ordinary expiry transition.
TEST(RevokeApi, a_pending_revoke_precedes_an_unrelated_due_expiry) {
    RevokeNode work;
    work.run();
    const uint64_t target = work.durable_grant(1);
    constexpr uint64_t lifetime_us = 1000000;
    const uint64_t expiring = work.durable_grant(2, 0, lifetime_us);
    work.node.sim().set_storage_capacity(0);
    const auto accepted = work.revoke(target, 3);
    ASSERT_EQ(accepted.status, AYNI_OK);
    ASSERT_EQ(accepted.result.phase, AYNI_INTENT_PENDING);
    work.node.sim().advance_us(lifetime_us);
    work.node.sim().set_storage_capacity(UINT64_MAX);
    EXPECT_EQ(work.node.step(1).items_done, 1U);
    EXPECT_EQ(work.grant(target).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(work.receipt(3).phase, AYNI_INTENT_APPLIED);
    EXPECT_EQ(work.grant(expiring).lifecycle, AYNI_GRANT_ACTIVE);
}
} // namespace
