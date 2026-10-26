#include "kernel/service/intent/intent.h"
// MA-006 C public behavior: management provenance, finite attenuation,
// durable parents, exact retries, and recovery without a renewed clock.
#include "kernel/service/model/work_model.h"
#include "work_node.h"

#include <ayni/authority.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using ayni::kernel::KernelTestProbe;

namespace {
using namespace ayni::test;

struct RangeScope {
    ayni_scope_field_v1 field{};
    ayni_scope_v1 scope{};
    RangeScope(int64_t minimum = 10, int64_t maximum = 20) {
        field.struct_size = sizeof(field);
        field.field_id = 10;
        field.allowed.struct_size = sizeof(field.allowed);
        field.allowed.kind = AYNI_AUTHORITY_I64;
        field.allowed.unit = {"count", 5};
        field.allowed.values.integer = {minimum, maximum};
        scope = {sizeof(scope), 1, &field};
    }
};

class GrantNode {
  public:
    GrantNode() {
        node.config().grant_capacity = 8;
        node.config().command_capacity = 16;
        node.on_create([this](ayni_kernel_t *kernel) {
            const auto owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            for (const uint64_t id : {agent_a, agent_b}) {
                if (id == agent_a && !with_agent_a) {
                    continue;
                }
                const auto agent = agent_desc(id);
                ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            }
            if (!with_capability) {
                return;
            }
            std::array<ayni_authority_field_desc_v1, 3> fields{};
            for (auto &field : fields) {
                field.struct_size = sizeof(field);
                field.domain.struct_size = sizeof(field.domain);
            }
            fields[0].field_id = 30;
            fields[0].usage = AYNI_AUTHORITY_PARAMETER;
            fields[0].domain.kind = AYNI_AUTHORITY_ENUM;
            const std::array<ayni_string_v1, 2> modes{text("slow"), text("fast")};
            fields[0].domain.values.tokens = {modes.data(), 2, 0};
            fields[1].field_id = 10;
            fields[1].usage = AYNI_AUTHORITY_PARAMETER;
            fields[1].domain.kind = AYNI_AUTHORITY_I64;
            fields[1].domain.unit = text("count");
            fields[1].domain.values.integer = {0, domain_maximum};
            fields[2].field_id = 20;
            fields[2].usage = AYNI_AUTHORITY_EXECUTION_TARGET;
            fields[2].domain.kind = AYNI_AUTHORITY_IDENTIFIER;
            const std::array<ayni_string_v1, 2> motors{text("motor.b"), text("motor.a")};
            fields[2].domain.values.tokens = {motors.data(), 2, 0};
            const ayni_capability_desc_v1 descriptor{sizeof(descriptor),
                                                     capability_version,
                                                     text("device.move"),
                                                     static_cast<uint32_t>(fields.size()),
                                                     0,
                                                     fields.data()};
            ASSERT_EQ(ayni_register_capability(kernel, &descriptor), AYNI_OK);
        });
    }

    void run() {
        node.run();
        reopen_session();
    }
    void reopen_session() { session = open_session(); }
    uint64_t open_session(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT) {
        const auto descriptor = session_desc(channel);
        uint64_t id = 0;
        EXPECT_EQ(ayni_open_session(node.kernel(), &descriptor, &id), AYNI_OK);
        return id;
    }
    uint64_t subject(uint64_t agent_id = agent_a) {
        ayni_agent_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_agent(node.kernel(), agent_id, &info), AYNI_OK);
        return info.instance_id;
    }
    ayni_grant_create_v1 body(const RangeScope &range, uint64_t parent = 0, uint32_t flags = 0,
                              uint64_t duration = 0) {
        ayni_grant_create_v1 result{};
        result.struct_size = sizeof(result);
        result.capability_version = 1;
        result.capability_id = text("device.move");
        result.subject_instance_id = subject();
        result.parent_grant_id = parent;
        result.lifetime = duration == 0U ? AYNI_GRANT_PERMANENT : AYNI_GRANT_CURRENT_BOOT;
        result.flags = flags;
        result.duration_us = duration;
        result.scope = range.scope;
        return result;
    }
    Submitted submit(const ayni_grant_create_v1 &body, uint64_t command, uint32_t expected = 0,
                     uint64_t through_session = 0) {
        const ayni_intent_v1 intent{
            sizeof(intent), AYNI_INTENT_GRANT_CREATE,
            command,        through_session == 0U ? session : through_session,
            expected,       0,
            &body};
        Submitted submitted{};
        submitted.result.struct_size = sizeof(submitted.result);
        submitted.error.struct_size = sizeof(submitted.error);
        submitted.status =
            ayni_submit_intent(node.kernel(), &intent, &submitted.result, &submitted.error);
        EXPECT_EQ(submitted.error.status, submitted.status);
        return submitted;
    }
    ayni_grant_info_v1 grant(uint64_t id) {
        ayni_grant_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_grant(node.kernel(), id, &info), AYNI_OK);
        return info;
    }
    ayni_grant_field_info_v1 field(uint64_t id, uint32_t index) {
        ayni_grant_field_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_grant_field(node.kernel(), id, index, &info), AYNI_OK);
        return info;
    }
    void store(uint64_t sequence) {
        for (unsigned round = 0; round < 1000 && node.info().durable_seq < sequence; ++round) {
            const auto report = node.step(1);
            const auto completed = node.sim().complete_storage();
            if (node.info().durable_seq >= sequence) {
                break;
            }
            if (completed != 0U) {
                continue; // apply the posted completion before advancing time
            }
            if (report.more_ready == 0U && node.sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.sim().now_us()) {
                node.sim().advance_us(report.next_deadline_us - node.sim().now_us());
            }
        }
        ASSERT_GE(node.info().durable_seq, sequence);
    }
    void settle_expiry(uint64_t id) {
        for (unsigned round = 0; round < 1000 && grant(id).lifecycle == AYNI_GRANT_ACTIVE;
             ++round) {
            node.step(1);
        }
        ASSERT_EQ(grant(id).lifecycle, AYNI_GRANT_EXPIRED);
    }

    Node node;
    bool with_capability = true;
    bool with_agent_a = true;
    uint32_t capability_version = 1;
    int64_t domain_maximum = 100;
    uint64_t session = 0;
};

// C-AUTH-021/022: finite authority cannot outlive its boot or parent.
TEST(GrantApi, creation_copies_finite_scope_and_typed_queries_expand_omitted_fields) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto body = work.body(range, 0, AYNI_GRANT_ALLOW_DELEGATION);
    const uint64_t before = work.node.info().durable_seq;
    const auto created = work.submit(body, 1);
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(created.result.object.kind, AYNI_OBJECT_GRANT);
    EXPECT_EQ(created.result.object.revision, 1U);
    EXPECT_GT(created.result.commit_seq, before);
    const uint64_t id = created.result.object.id;
    auto info = work.grant(id);
    EXPECT_EQ(info.subject_instance_id, work.subject());
    EXPECT_EQ(info.owner_principal_id, owner_id);
    EXPECT_EQ(info.owner_binding_revision, owner_binding);
    EXPECT_EQ(std::string(info.capability_id, info.capability_id_size), "device.move");
    EXPECT_EQ(info.capability_version, 1U);
    EXPECT_EQ(info.field_count, 3U);
    EXPECT_EQ(info.depth, 0U);
    EXPECT_EQ(info.lifetime, AYNI_GRANT_PERMANENT);
    EXPECT_EQ(info.deadline_us, 0U);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GRANT_NOT_DURABLE);
    range.field.allowed.values.integer = {90, 99};
    const auto integer = work.field(id, 0);
    EXPECT_EQ(integer.field_id, 10U);
    EXPECT_EQ(integer.kind, AYNI_AUTHORITY_I64);
    EXPECT_EQ(std::string(integer.unit, integer.unit_size), "count");
    EXPECT_EQ(integer.values.integer.min_inclusive, 10);
    EXPECT_EQ(integer.values.integer.max_inclusive, 20);
    const auto target = work.field(id, 1);
    EXPECT_EQ(target.field_id, 20U);
    EXPECT_EQ(target.kind, AYNI_AUTHORITY_IDENTIFIER);
    EXPECT_EQ(target.usage, AYNI_AUTHORITY_EXECUTION_TARGET);
    ASSERT_EQ(target.values.tokens.member_count, 2U);
    EXPECT_EQ(std::string(target.values.tokens.members[0], target.values.tokens.member_sizes[0]),
              "motor.a");
    EXPECT_EQ(std::string(target.values.tokens.members[1], target.values.tokens.member_sizes[1]),
              "motor.b");
    const auto mode = work.field(id, 2);
    EXPECT_EQ(mode.kind, AYNI_AUTHORITY_ENUM);
    EXPECT_EQ(std::string(mode.values.tokens.members[0], mode.values.tokens.member_sizes[0]),
              "fast");
    work.store(created.result.commit_seq);
    info = work.grant(id);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_NONE);
    EXPECT_EQ(info.commit_seq, created.result.commit_seq);
}

// ADR-0030 decision 2: typed query outputs publish only a complete successful result.
TEST(GrantApi, malformed_queries_never_publish_a_partial_output) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto created = work.submit(work.body(range), 1);
    ASSERT_EQ(created.status, AYNI_OK);
    ayni_grant_info_v1 info{};
    std::memset(&info, 0xA5, sizeof(info));
    info.struct_size = sizeof(info);
    const auto original = info;
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), UINT64_MAX, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(std::memcmp(&info, &original, sizeof(info)), 0);
    EXPECT_EQ(ayni_get_grant(nullptr, created.result.object.id, &info), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), created.result.object.id, nullptr),
              AYNI_INVALID_ARGUMENT);
    info.struct_size += 1;
    const auto oversized = info;
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), created.result.object.id, &info),
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(std::memcmp(&info, &oversized, sizeof(info)), 0);
    ayni_grant_field_info_v1 field{};
    std::memset(&field, 0xA5, sizeof(field));
    field.struct_size = sizeof(field);
    const auto original_field = field;
    EXPECT_EQ(ayni_get_grant_field(work.node.kernel(), created.result.object.id, 3, &field),
              AYNI_NOT_FOUND);
    EXPECT_EQ(std::memcmp(&field, &original_field, sizeof(field)), 0);
}

// C-AUTH-019 / ADR-0030 decision 1: trusted bounded admission precedes allocation/publication.
TEST(GrantApi, new_commands_require_management_nonzero_command_and_explicit_supported_features) {
    GrantNode work;
    work.run();
    RangeScope range;
    auto body = work.body(range);
    EXPECT_EQ(work.submit(body, 0).status, AYNI_INVALID_ARGUMENT);
    const uint64_t voice = work.open_session(AYNI_CHANNEL_VOICE);
    const auto denied = work.submit(body, 1, 0, voice);
    EXPECT_EQ(denied.status, AYNI_DENIED);
    EXPECT_EQ(denied.error.reason, AYNI_REASON_GRANT_CHANNEL);
    body.flags = AYNI_GRANT_REQUIRES_SCENE;
    EXPECT_EQ(work.submit(body, 1).status, AYNI_UNSUPPORTED);
    body.flags = AYNI_GRANT_REQUIRES_QUOTA;
    EXPECT_EQ(work.submit(body, 1).status, AYNI_UNSUPPORTED);
    body.flags = 8;
    EXPECT_EQ(work.submit(body, 1).status, AYNI_INVALID_ARGUMENT);
    body.flags = 0;
    const auto accepted = work.submit(body, 1);
    ASSERT_EQ(accepted.status, AYNI_OK);
    const auto retried = work.submit(body, 1, 0, voice);
    ASSERT_EQ(retried.status, AYNI_OK);
    EXPECT_EQ(retried.result.object.id, accepted.result.object.id);
    EXPECT_EQ(retried.result.commit_seq, accepted.result.commit_seq);
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, delegation_requires_a_durable_parent_revision_and_one_covering_scope) {
    GrantNode work;
    work.run();
    RangeScope parent(10, 20);
    const auto created = work.submit(work.body(parent, 0, AYNI_GRANT_ALLOW_DELEGATION), 1);
    ASSERT_EQ(created.status, AYNI_OK);
    RangeScope child(11, 19);
    const auto child_body = work.body(child, created.result.object.id);
    const auto pending = work.submit(child_body, 2, 1);
    EXPECT_EQ(pending.status, AYNI_BUSY);
    EXPECT_EQ(pending.error.reason, AYNI_REASON_GRANT_NOT_DURABLE);
    work.store(created.result.commit_seq);
    EXPECT_EQ(work.submit(child_body, 2, 2).status, AYNI_REVISION_CONFLICT);
    RangeScope too_wide(9, 19);
    const auto expanded = work.submit(work.body(too_wide, created.result.object.id), 2, 1);
    EXPECT_EQ(expanded.status, AYNI_DENIED);
    EXPECT_EQ(expanded.error.reason, AYNI_REASON_GRANT_SCOPE);
    const auto accepted = work.submit(child_body, 2, 1);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(work.grant(accepted.result.object.id).depth, 1U);
    EXPECT_EQ(work.grant(accepted.result.object.id).parent_grant_id, created.result.object.id);
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, stopping_an_ancestor_subject_invalidates_a_child_owned_by_another_live_subject) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto parent = work.submit(work.body(range, 0, AYNI_GRANT_ALLOW_DELEGATION), 1);
    ASSERT_EQ(parent.status, AYNI_OK);
    work.store(parent.result.commit_seq);
    auto child_body = work.body(range, parent.result.object.id, AYNI_GRANT_ALLOW_DELEGATION);
    child_body.subject_instance_id = work.subject(agent_b);
    const auto child = work.submit(child_body, 2, 1);
    ASSERT_EQ(child.status, AYNI_OK);
    work.store(child.result.commit_seq);
    const ayni_instance_target_v1 target{sizeof(target), 0, work.subject(agent_a)};
    const ayni_intent_v1 intent{sizeof(intent), AYNI_INTENT_INSTANCE_STOP, 3, work.session, 1, 0,
                                &target};
    ayni_intent_result_v1 result{};
    result.struct_size = sizeof(result);
    ayni_error_v1 error{};
    error.struct_size = sizeof(error);
    ASSERT_EQ(ayni_submit_intent(work.node.kernel(), &intent, &result, &error), AYNI_OK);
    const auto held = work.grant(child.result.object.id);
    EXPECT_EQ(held.lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(held.revision, 1U);
    EXPECT_EQ(held.effective_reason, AYNI_REASON_GRANT_SUBJECT);
    auto descendant = work.body(range, child.result.object.id);
    descendant.subject_instance_id = work.subject(agent_b);
    const auto denied = work.submit(descendant, 4, 1);
    EXPECT_EQ(denied.status, AYNI_DENIED);
    EXPECT_EQ(denied.error.reason, AYNI_REASON_GRANT_SUBJECT);
    const auto retry = work.submit(child_body, 2, 1);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, child.result.commit_seq);
    descendant.parent_grant_id = 0;
    EXPECT_EQ(work.submit(descendant, 4).status, AYNI_OK);
}

// C-AUTH-021/022: finite authority cannot outlive its boot or parent.
TEST(GrantApi, arrival_at_deadline_denies_immediately_before_step_records_the_terminal_state) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto created = work.submit(work.body(range, 0, AYNI_GRANT_ALLOW_DELEGATION, 10000000), 1);
    ASSERT_EQ(created.status, AYNI_OK);
    work.store(created.result.commit_seq);
    auto info = work.grant(created.result.object.id);
    ASSERT_GT(info.deadline_us, work.node.sim().now_us());
    work.node.sim().advance_us(info.deadline_us - work.node.sim().now_us());
    info = work.grant(created.result.object.id);
    EXPECT_EQ(info.lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(info.revision, 1U);
    EXPECT_EQ(info.effective_reason, AYNI_REASON_GRANT_EXPIRED);
    const auto denied = work.submit(work.body(range, created.result.object.id, 0, 1), 2, 1);
    EXPECT_EQ(denied.status, AYNI_DENIED);
    EXPECT_EQ(denied.error.reason, AYNI_REASON_GRANT_EXPIRED);
    work.settle_expiry(created.result.object.id);
    info = work.grant(created.result.object.id);
    EXPECT_EQ(info.revision, 2U);
    EXPECT_GT(info.commit_seq, created.result.commit_seq);
    work.store(info.commit_seq);
    const auto retry = work.submit(work.body(range, 0, AYNI_GRANT_ALLOW_DELEGATION, 10000000), 1);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, created.result.commit_seq);
}

// C-AUTH-021/022: finite authority cannot outlive its boot or parent.
TEST(GrantApi, restart_keeps_permanent_grants_and_expires_finite_grants_without_reanchoring) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto permanent = work.submit(work.body(range), 1);
    const auto limited = work.submit(work.body(range, 0, 0, UINT64_C(1000000000000)), 2);
    ASSERT_EQ(permanent.status, AYNI_OK);
    ASSERT_EQ(limited.status, AYNI_OK);
    work.node.reboot();
    work.node.recover();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    const auto kept = work.grant(permanent.result.object.id);
    EXPECT_EQ(kept.effective_reason, AYNI_REASON_NONE);
    EXPECT_EQ(kept.created_commit_seq, permanent.result.commit_seq);
    const auto expired = work.grant(limited.result.object.id);
    EXPECT_EQ(expired.lifetime, AYNI_GRANT_CURRENT_BOOT);
    EXPECT_EQ(expired.duration_us, UINT64_C(1000000000000));
    EXPECT_EQ(expired.deadline_us, 0U);
    EXPECT_EQ(expired.lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(expired.revision, 2U);
    EXPECT_EQ(expired.effective_reason, AYNI_REASON_GRANT_EXPIRED);
    EXPECT_EQ(expired.created_commit_seq, limited.result.commit_seq);
    EXPECT_GT(expired.commit_seq, limited.result.commit_seq);
}

// C-AUTH-020/022: exact saved catalog semantics bound one-parent delegation.
TEST(GrantApi,
     missing_version_or_changed_catalog_holds_facts_but_exact_retry_precedes_current_semantics) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto body = work.body(range);
    const auto created = work.submit(body, 1);
    ASSERT_EQ(created.status, AYNI_OK);
    for (const uint32_t mode : {0U, 1U, 2U, 3U}) {
        work.with_capability = mode != 0U;
        work.capability_version = mode == 1U ? 2U : 1U;
        work.domain_maximum = mode == 2U ? 101 : 100;
        work.node.reboot();
        work.node.recover();
        ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        work.reopen_session();
        const auto info = work.grant(created.result.object.id);
        const ayni_reason_t expected = mode == 0U   ? AYNI_REASON_CAPABILITY_UNKNOWN
                                       : mode == 1U ? AYNI_REASON_CAPABILITY_VERSION
                                       : mode == 2U ? AYNI_REASON_GRANT_CATALOG_CHANGED
                                                    : AYNI_REASON_NONE;
        EXPECT_EQ(info.effective_reason, expected) << mode;
        EXPECT_EQ(info.lifecycle, AYNI_GRANT_ACTIVE);
        EXPECT_EQ(info.revision, 1U);
        EXPECT_EQ(info.commit_seq, created.result.commit_seq);
        const auto retry = work.submit(body, 1);
        ASSERT_EQ(retry.status, AYNI_OK) << mode;
        EXPECT_EQ(retry.result.object.id, created.result.object.id);
        EXPECT_EQ(retry.result.commit_seq, created.result.commit_seq);
    }
}

// C-AUTH-023 / ADR-0030 decision 7: only durable terminal leaves may make room.
TEST(GrantApi, only_a_durable_expired_leaf_can_make_room_and_retry_keeps_its_removed_reference) {
    GrantNode work;
    work.node.config().grant_capacity = 1;
    work.run();
    RangeScope range;
    const auto finite_body = work.body(range, 0, 0, 10000000);
    const auto created = work.submit(finite_body, 1);
    ASSERT_EQ(created.status, AYNI_OK);
    work.store(created.result.commit_seq);
    const auto before = work.grant(created.result.object.id);
    work.node.sim().advance_us(before.deadline_us - work.node.sim().now_us());
    EXPECT_EQ(work.submit(work.body(range), 2).status, AYNI_CAPACITY_EXCEEDED);
    work.settle_expiry(created.result.object.id);
    const auto terminal = work.grant(created.result.object.id);
    ASSERT_GT(terminal.commit_seq, work.node.info().durable_seq);
    EXPECT_EQ(work.submit(work.body(range), 2).status, AYNI_CAPACITY_EXCEEDED);
    work.store(terminal.commit_seq);
    const auto replacement = work.submit(work.body(range), 2);
    ASSERT_EQ(replacement.status, AYNI_OK);
    EXPECT_NE(replacement.result.object.id, created.result.object.id);
    ayni_grant_info_v1 old{};
    old.struct_size = sizeof(old);
    EXPECT_EQ(ayni_get_grant(work.node.kernel(), created.result.object.id, &old), AYNI_NOT_FOUND);
    const auto retry = work.submit(finite_body, 1);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.object.id, created.result.object.id);
    EXPECT_EQ(retry.result.commit_seq, created.result.commit_seq);
}

// ADR-0030 decision 8: recovery validates complete related facts before publication.
TEST(GrantApi, interrupted_append_recovers_both_create_and_exact_mapping_or_neither) {
    for (const auto cut :
         {ayni::hosts::sim::CutAppend::lost, ayni::hosts::sim::CutAppend::stored}) {
        GrantNode work;
        work.run();
        RangeScope range;
        const auto body = work.body(range);
        const auto created = work.submit(body, 1);
        ASSERT_EQ(created.status, AYNI_OK);
        for (unsigned round = 0; round < 1000 && work.node.sim().pending_storage() == 0U; ++round) {
            const auto report = work.node.step(1);
            if (report.more_ready == 0U && report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > work.node.sim().now_us()) {
                work.node.sim().advance_us(report.next_deadline_us - work.node.sim().now_us());
            }
        }
        ASSERT_NE(work.node.sim().pending_storage(), 0U);
        work.node.power_cut_and_start(cut);
        work.node.recover();
        ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        work.reopen_session();
        ayni_grant_info_v1 original{};
        original.struct_size = sizeof(original);
        EXPECT_EQ(ayni_get_grant(work.node.kernel(), created.result.object.id, &original),
                  cut == ayni::hosts::sim::CutAppend::stored ? AYNI_OK : AYNI_NOT_FOUND);
        const auto retried = work.submit(body, 1);
        ASSERT_EQ(retried.status, AYNI_OK);
        if (cut == ayni::hosts::sim::CutAppend::stored) {
            EXPECT_EQ(retried.result.object.id, created.result.object.id);
            EXPECT_EQ(retried.result.commit_seq, created.result.commit_seq);
        } else {
            EXPECT_NE(retried.result.object.id, created.result.object.id);
            EXPECT_GT(retried.result.commit_seq, created.result.commit_seq);
        }
    }
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, unknown_exact_capability_and_subject_are_refused_before_committing_or_mapping) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto valid = work.body(range);
    const auto before = KernelTestProbe::work(work.node.kernel()).transitions.next_commit_seq();
    auto body = valid;
    body.capability_id = text("missing.capability");
    const auto unknown = work.submit(body, 1);
    EXPECT_EQ(unknown.status, AYNI_NOT_FOUND);
    EXPECT_EQ(unknown.error.reason, AYNI_REASON_CAPABILITY_UNKNOWN);
    body = valid;
    body.capability_version = 2;
    const auto version = work.submit(body, 1);
    EXPECT_EQ(version.status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(version.error.reason, AYNI_REASON_CAPABILITY_VERSION);
    body = valid;
    body.subject_instance_id = UINT64_MAX;
    const auto subject = work.submit(body, 1);
    EXPECT_EQ(subject.status, AYNI_DENIED);
    EXPECT_EQ(subject.error.reason, AYNI_REASON_GRANT_SUBJECT);
    EXPECT_EQ(subject.error.object.id, UINT64_MAX);
    EXPECT_EQ(KernelTestProbe::work(work.node.kernel()).transitions.next_commit_seq(), before);
    EXPECT_EQ(work.submit(valid, 1).status, AYNI_OK);
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, a_stopped_or_unregistered_subject_cannot_receive_a_new_grant) {
    for (const bool stopped : {false, true}) {
        GrantNode work;
        work.run();
        if (stopped) {
            const ayni_instance_target_v1 target{sizeof(target), 0, work.subject()};
            const ayni_intent_v1 intent{
                sizeof(intent), AYNI_INTENT_INSTANCE_STOP, 10, work.session, 1, 0, &target};
            ayni_error_v1 error{};
            error.struct_size = sizeof(error);
            ASSERT_EQ(ayni_submit_intent(work.node.kernel(), &intent, nullptr, &error), AYNI_OK);
        } else {
            work.with_agent_a = false;
            work.node.reboot();
            work.node.recover();
            ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
            work.reopen_session();
        }
        RangeScope range;
        auto body = work.body(range);
        const auto denied = work.submit(body, 11);
        EXPECT_EQ(denied.status, AYNI_DENIED);
        EXPECT_EQ(denied.error.reason, AYNI_REASON_GRANT_SUBJECT);
        body.subject_instance_id = work.subject(agent_b);
        EXPECT_EQ(work.submit(body, 11).status, AYNI_OK);
    }
}

// C-AUTH-019 / ADR-0030 decision 1: trusted bounded admission precedes allocation/publication.
TEST(GrantApi, kernel_semantic_boundary_rejects_unproven_or_stale_owner_evidence) {
    GrantNode work;
    work.run();
    RangeScope range;
    ayni::kernel::Intent intent{};
    intent.kind = AYNI_INTENT_GRANT_CREATE;
    intent.command_id = 1;
    intent.grant_create = work.body(range);
    auto &model = KernelTestProbe::work(work.node.kernel());
    const auto current = *model.sessions.find(work.session);
    const auto before = model.transitions.next_commit_seq();
    // White-box Kernel-module trust-boundary fault injection. Public sessions
    // never manufacture these facts; production must still refuse stale input
    // before the authority table, allocator or transition clock changes.
    const ayni_host_v1 &host = KernelTestProbe::host(work.node.kernel());
    const auto apply = [&](ayni::kernel::WorkModel &target, const ayni::kernel::Intent &request,
                           uint64_t durable, ayni::kernel::IntentFailure &failure) {
        ayni::kernel::IntentOutcome outcome{};
        const ayni::kernel::IntentContext context{
            work.node.sim().now_us(),
            durable,
            true,
            AYNI_OK,
            {host.ctx, host.monotonic_now_us},
            {&KernelTestProbe::ingress(work.node.kernel()), [](void *ingress) {
                 return static_cast<ayni::kernel::EventIngress *>(ingress)->resume_boundary();
             }}};
        return ayni::kernel::apply_intent(target, request, context, outcome, failure);
    };
    for (uint32_t fault = 0; fault < 3; ++fault) {
        auto session = current;
        session.session_id = current.session_id + 1U + fault; // not opened through the ABI
        uint64_t durable = work.node.info().durable_seq;
        if (fault == 0U) {
            session.principal_id += 1U;
        } else if (fault == 1U) {
            session.binding_revision += 1U;
        } else {
            durable = model.principals.binding()->stamp.commit_seq - 1U;
        }
        ASSERT_TRUE(model.sessions.open(session));
        intent.session_id = session.session_id;
        ayni::kernel::IntentFailure failure{};
        EXPECT_EQ(apply(model, intent, durable, failure), AYNI_DENIED) << fault;
        EXPECT_EQ(failure.reason, AYNI_REASON_GRANT_CHANNEL);
        EXPECT_EQ(model.grants.size(), 0U);
        EXPECT_EQ(model.transitions.next_commit_seq(), before);
        ASSERT_TRUE(model.sessions.close(session.session_id));
    }
    Node unbound;
    unbound.create();
    auto &unbound_model = KernelTestProbe::work(unbound.kernel());
    ASSERT_TRUE(unbound_model.sessions.open(current));
    intent.session_id = current.session_id;
    ayni::kernel::IntentFailure failure{};
    EXPECT_EQ(apply(unbound_model, intent, 0, failure), AYNI_DENIED);
    EXPECT_EQ(failure.reason, AYNI_REASON_GRANT_CHANNEL);
    EXPECT_EQ(unbound_model.grants.size(), 0U);
}

// C-AUTH-019 / ADR-0030 decision 1: trusted bounded admission precedes allocation/publication.
TEST(GrantApi, id_exhaustion_refuses_a_new_grant_and_does_not_accept_its_command) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto body = work.body(range);
    auto &model = KernelTestProbe::work(work.node.kernel());
    const uint64_t boot = work.node.info().boot_epoch;
    // White-box boundary placement avoids allocating four billion objects.
    model.ids.start(boot, UINT32_MAX);
    uint64_t last = 0;
    ASSERT_TRUE(model.ids.allocate(last));
    const uint64_t before = model.transitions.next_commit_seq();
    const auto refused = work.submit(body, 1);
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_ID_EXHAUSTED);
    EXPECT_EQ(model.transitions.next_commit_seq(), before);
    model.ids.start(boot, 100);
    const auto accepted = work.submit(body, 1);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(accepted.result.object.id, (boot << 32U) | 100U);
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, restored_faulted_ancestor_subject_is_refused_by_the_shared_kernel_chain_check) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto parent = work.submit(work.body(range, 0, AYNI_GRANT_ALLOW_DELEGATION), 1);
    ASSERT_EQ(parent.status, AYNI_OK);
    work.store(parent.result.commit_seq);
    auto child_body = work.body(range, parent.result.object.id, AYNI_GRANT_ALLOW_DELEGATION);
    child_body.subject_instance_id = work.subject(agent_b);
    const auto child = work.submit(child_body, 2, 1);
    ASSERT_EQ(child.status, AYNI_OK);
    work.with_agent_a = false;
    work.node.reboot();
    work.node.recover();
    ASSERT_EQ(work.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    const auto held = work.grant(child.result.object.id);
    EXPECT_EQ(held.effective_reason, AYNI_REASON_GRANT_SUBJECT);
    auto descendant = work.body(range, child.result.object.id);
    descendant.subject_instance_id = work.subject(agent_b);
    const auto denied = work.submit(descendant, 3, 1);
    EXPECT_EQ(denied.status, AYNI_DENIED);
    EXPECT_EQ(denied.error.reason, AYNI_REASON_GRANT_SUBJECT);
}

// C-AUTH-019/022: authority and usable subjects must hold over the complete chain.
TEST(GrantApi, shared_kernel_check_denies_missing_or_newly_faulted_subject_facts) {
    GrantNode work;
    work.run();
    RangeScope range;
    const auto created = work.submit(work.body(range), 1);
    ASSERT_EQ(created.status, AYNI_OK);
    work.store(created.result.commit_seq);
    auto &model = KernelTestProbe::work(work.node.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(model.grants.find(created.result.object.id, index));
    const auto subject_id = work.subject();
    auto *subject = const_cast<ayni::work::AgentInstance *>(model.agents.find_instance(subject_id));
    ASSERT_NE(subject, nullptr);
    // White-box runtime fault injection: check_grant must consult the current
    // work owner instead of assuming the creation-time subject stays usable.
    const auto previous_lifecycle = subject->lifecycle;
    subject->lifecycle = AYNI_AGENT_FAULTED;
    EXPECT_EQ(
        model.check_grant(index, work.node.sim().now_us(), work.node.info().durable_seq).reason,
        AYNI_REASON_GRANT_SUBJECT);
    subject->lifecycle = previous_lifecycle;
    const auto previous_id = subject->instance_id;
    subject->instance_id = UINT64_MAX;
    EXPECT_EQ(
        model.check_grant(index, work.node.sim().now_us(), work.node.info().durable_seq).reason,
        AYNI_REASON_GRANT_SUBJECT);
    subject->instance_id = previous_id;
    EXPECT_EQ(
        model.check_grant(index, work.node.sim().now_us(), work.node.info().durable_seq).status,
        AYNI_OK);
}

} // namespace
