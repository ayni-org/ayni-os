#include "authority/grant/grant_records.h"
#include "authority/grant/grant_table.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
using namespace ayni::authority;
using ayni::foundation::TransitionStamp;

constexpr uint64_t id(uint32_t counter, uint64_t boot = 1) {
    return (boot << 32U) | counter;
}
constexpr GrantContext ready{1, 10, 1000};
constexpr std::array<uint8_t, 4> domain_bytes{1, 0, 0, 0};

class Grants {
  public:
    explicit Grants(uint32_t capacity = 8)
        : memory_(GrantTable::storage_size({capacity, 64, 4}) / sizeof(uint64_t) + 1U),
          table(memory_.data(), {capacity, 64, 4}) {}
    NewGrant input(uint64_t parent = 0, uint64_t duration = 0) const {
        ayni::catalog::CatalogId capability;
        EXPECT_TRUE(capability.assign({"cap", 3}));
        return {id(100),
                parent,
                7,
                3,
                capability,
                1,
                {domain_bytes.data(), static_cast<uint32_t>(domain_bytes.size())},
                &empty_,
                duration == 0U ? AYNI_GRANT_PERMANENT : AYNI_GRANT_CURRENT_BOOT,
                duration,
                AYNI_GRANT_ALLOW_DELEGATION};
    }
    bool add(uint32_t counter, uint64_t parent = 0, uint64_t duration = 0, uint64_t sequence = 0) {
        const auto request = input(parent, duration);
        GrantTable::Admission admission{};
        const auto check = table.check_create(request, parent == 0U ? 0U : 1U, ready, admission);
        if (check.status != AYNI_OK) {
            return false;
        }
        uint32_t index = 0;
        const uint64_t seq = sequence == 0U ? counter : sequence;
        return table.insert(request, admission, id(counter), {seq, seq}, index);
    }
    uint32_t index(uint64_t grant_id) const {
        uint32_t index = 0;
        EXPECT_TRUE(table.find(grant_id, index));
        return index;
    }
    GrantRecord record(uint32_t index) const {
        const Grant &grant = table.at(index);
        NewGrant fact{grant.subject_instance_id,
                      grant.parent_grant_id,
                      grant.owner_principal_id,
                      grant.owner_binding_revision,
                      grant.capability_id,
                      grant.capability_version,
                      table.source_domain(index),
                      nullptr,
                      grant.lifetime,
                      grant.duration_us,
                      grant.allow_delegation ? AYNI_GRANT_ALLOW_DELEGATION : 0U};
        return {grant.grant_id, grant.replaced_grant_id, grant.depth, fact, table.scope(index)};
    }
    GrantSnapshotRecord snapshot(uint32_t index) const {
        const Grant &grant = table.at(index);
        return {record(index),
                grant.created_stamp,
                grant.stamp,
                grant.revision,
                grant.lifecycle,
                grant.revoked_by_root_id,
                grant.cleanup_after_grant_id,
                grant.cleanup_through_grant_id,
                grant.cleanup_stamp};
    }
    void revoke(uint64_t root_id, uint64_t seq) {
        EXPECT_TRUE(table.apply_revoke(table.plan_accepted_revoke(root_id), {seq, seq}));
    }
    void clean(uint64_t &seq) {
        GrantTable::CleanupPlan next{};
        uint32_t bound = 0;
        while (table.next_revocation_cleanup(next)) {
            ASSERT_LT(bound++, table.size() * table.size() + table.size());
            ++seq;
            ASSERT_TRUE(table.apply_cleanup(next, {seq, seq}));
        }
    }

  private:
    std::vector<uint64_t> memory_;
    const ayni_scope_v1 empty_{sizeof(ayni_scope_v1), 0, nullptr};

  public:
    GrantTable table;
};

// C-AUTH-024: accepted volatile control denies before persistence and survives reload.
TEST(GrantRevoke, gate_denies_every_ancestor_before_other_facts_and_survives_restored_clear) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1)));
    ASSERT_TRUE(grants.add(3, id(2)));
    const auto grandchild = grants.index(id(3));
    EXPECT_EQ(grants.table.check_chain(grandchild, ready).status, AYNI_OK);
    ASSERT_TRUE(grants.table.install_gate(id(1)));
    EXPECT_EQ(grants.table.gate_count(), 1U);
    EXPECT_EQ(grants.table.at(0).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(grants.table.check_chain(grandchild, ready).reason, AYNI_REASON_GRANT_REVOKED);
    GrantTable::Admission admission{};
    EXPECT_EQ(grants.table.check_create(grants.input(id(3)), 1, ready, admission).status,
              AYNI_DENIED);
    auto parent = grants.record(0);
    parent.grant.source_domain = {domain_bytes.data(), static_cast<uint32_t>(domain_bytes.size())};
    parent.scope = parent.grant.source_domain;
    grants.table.clear_restored();
    EXPECT_EQ(grants.table.size(), 0U);
    EXPECT_TRUE(grants.table.gated(id(1)));
    ASSERT_EQ(grants.table.restore_create(parent, {1, 1}), GrantTable::Restore::restored);
    EXPECT_EQ(grants.table.check_chain(0, ready).reason, AYNI_REASON_GRANT_REVOKED);
    ASSERT_TRUE(grants.table.release_gate(id(1)));
    EXPECT_EQ(grants.table.check_chain(0, ready).status, AYNI_OK);
    EXPECT_FALSE(grants.table.release_gate(id(1)));
    grants.table.install_gate(id(1));
    grants.table.clear();
    EXPECT_EQ(grants.table.gate_count(), 0U);
}

// C-AUTH-024: accepted volatile control denies before persistence and survives reload.
TEST(GrantRevoke, gate_capacity_is_independent_of_target_presence_and_duplicates_are_idempotent) {
    Grants zero(0);
    EXPECT_FALSE(zero.table.can_gate(id(1)));
    EXPECT_FALSE(zero.table.install_gate(id(1)));
    Grants grants(2);
    EXPECT_FALSE(grants.table.can_gate(0));
    EXPECT_FALSE(grants.table.install_gate(1));
    EXPECT_FALSE(grants.table.install_gate(id(0)));
    ASSERT_TRUE(grants.table.install_gate(id(10)));
    ASSERT_TRUE(grants.table.install_gate(id(20)));
    EXPECT_TRUE(grants.table.install_gate(id(10)));
    EXPECT_EQ(grants.table.gate_count(), 2U);
    EXPECT_FALSE(grants.table.can_gate(id(30)));
    EXPECT_FALSE(grants.table.install_gate(id(30)));
    ASSERT_TRUE(grants.table.release_gate(id(10)));
    EXPECT_TRUE(grants.table.gated(id(20)));
    EXPECT_TRUE(grants.table.install_gate(id(30)));
}

// C-AUTH-027: accepted control applies against current facts without repeating its CAS.
TEST(GrantRevoke, first_acceptance_checks_revision_without_requiring_durable_or_usable_target) {
    Grants grants;
    ASSERT_TRUE(grants.add(1, 0, 10, 10));
    EXPECT_EQ(grants.table.check_revoke(0, 1).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(grants.table.check_revoke(1, 1).status, AYNI_NOT_FOUND);
    EXPECT_EQ(grants.table.check_revoke(id(1), 0).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(grants.table.check_revoke(id(2), 1).status, AYNI_NOT_FOUND);
    EXPECT_EQ(grants.table.check_revoke(id(1), 2).status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(grants.table.check_chain(0, {1, 10, 0}).status, AYNI_BUSY);
    EXPECT_EQ(grants.table.check_revoke(id(1), 1).status, AYNI_OK);
    grants.table.hold_restored(0, AYNI_REASON_GRANT_CATALOG_CHANGED);
    EXPECT_EQ(grants.table.check_revoke(id(1), 1).status, AYNI_OK);
    grants.table.expire(0, {20, 20});
    EXPECT_EQ(grants.table.check_revoke(id(1), 1).status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(grants.table.check_revoke(id(1), 2).status, AYNI_OK);
}

// C-AUTH-027: accepted control applies against current facts without repeating its CAS.
TEST(GrantRevoke, accepted_revoke_plans_changed_no_change_and_target_gone_without_mutation) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(3));
    const auto changed = grants.table.plan_accepted_revoke(id(1));
    EXPECT_EQ(changed.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(changed.revision, 2U);
    EXPECT_EQ(changed.cleanup_after_grant_id, 0U);
    EXPECT_EQ(changed.cleanup_through_grant_id, id(3));
    EXPECT_EQ(grants.table.at(0).revision, 1U);
    auto wrong = changed;
    wrong.cleanup_through_grant_id = id(2);
    EXPECT_FALSE(grants.table.apply_revoke(wrong, {10, 10}));
    EXPECT_FALSE(grants.table.apply_revoke(changed, {0, 10}));
    EXPECT_FALSE(grants.table.apply_revoke(changed, {1, 1}));
    ASSERT_TRUE(grants.table.apply_revoke(changed, {10, 10}));
    EXPECT_EQ(grants.table.at(0).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(grants.table.at(0).revoked_by_root_id, id(1));
    EXPECT_EQ(grants.table.at(0).cleanup_stamp.commit_seq, 10U);
    EXPECT_EQ(grants.table.check_chain(0, ready).reason, AYNI_REASON_GRANT_REVOKED);
    EXPECT_FALSE(grants.table.apply_revoke(changed, {11, 11}));
    const auto noop = grants.table.plan_accepted_revoke(id(1));
    EXPECT_EQ(noop.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    EXPECT_EQ(noop.cleanup_through_grant_id, 0U);
    ASSERT_TRUE(grants.table.apply_revoke(noop, {11, 11}));
    EXPECT_EQ(grants.table.at(0).stamp.commit_seq, 10U);
    EXPECT_EQ(grants.table.at(0).cleanup_stamp.commit_seq, 10U);
    const auto absent = grants.table.plan_accepted_revoke(id(2));
    EXPECT_EQ(absent.effect, AYNI_INTENT_EFFECT_TARGET_GONE);
    EXPECT_EQ(absent.revision, 0U);
    ASSERT_TRUE(grants.table.apply_revoke(absent, {12, 12}));
    EXPECT_EQ(grants.table.size(), 2U);
    EXPECT_FALSE(grants.table.apply_revoke(grants.table.plan_accepted_revoke(1), {13, 13}));
}

// C-AUTH-027: accepted control applies against current facts without repeating its CAS.
TEST(GrantRevoke, expiry_between_acceptance_and_apply_is_no_change_and_preserves_terminal_order) {
    Grants grants;
    ASSERT_TRUE(grants.add(1, 0, 10));
    ASSERT_EQ(grants.table.check_revoke(id(1), 1).status, AYNI_OK);
    ASSERT_TRUE(grants.table.install_gate(id(1)));
    grants.table.expire(0, {10, 10});
    const auto result = grants.table.plan_accepted_revoke(id(1));
    EXPECT_EQ(result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    EXPECT_TRUE(grants.table.apply_revoke(result, {11, 11}));
    EXPECT_EQ(grants.table.at(0).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(grants.table.at(0).stamp.commit_seq, 10U);
    EXPECT_EQ(grants.table.at(0).cleanup_stamp.commit_seq, 0U);
    GrantTable::CleanupPlan next{};
    EXPECT_FALSE(grants.table.next_revocation_cleanup(next));
    EXPECT_EQ(grants.table.check_chain(0, ready).reason, AYNI_REASON_GRANT_REVOKED);
    grants.table.release_gate(id(1));
    EXPECT_EQ(grants.table.check_chain(0, ready).reason, AYNI_REASON_GRANT_EXPIRED);
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, cleanup_scans_ids_instead_of_slots_skips_unrelated_and_keeps_terminal_stamp) {
    Grants grants;
    ASSERT_TRUE(grants.add(4, 0, 0, 1));
    ASSERT_TRUE(grants.add(1, 0, 0, 2));
    ASSERT_TRUE(grants.add(3, id(1), 0, 3));
    ASSERT_TRUE(grants.add(2, 0, 0, 4));
    grants.revoke(id(1), 10);
    GrantTable::CleanupPlan next{};
    for (uint32_t counter = 1; counter <= 4; ++counter) {
        ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
        EXPECT_EQ(next.after_grant_id, id(counter));
        EXPECT_EQ(next.through_grant_id, id(4));
        EXPECT_EQ(next.revoked_grant_id, counter == 3U ? id(3) : 0U);
        auto stale = next;
        stale.after_grant_id += 1;
        EXPECT_FALSE(grants.table.apply_cleanup(stale, {10 + counter, 10 + counter}));
        ASSERT_TRUE(grants.table.apply_cleanup(next, {10 + counter, 10 + counter}));
        EXPECT_EQ(grants.table.at(grants.index(id(1))).stamp.commit_seq, 10U);
    }
    EXPECT_FALSE(grants.table.next_revocation_cleanup(next));
    EXPECT_EQ(grants.table.at(grants.index(id(3))).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(grants.table.at(grants.index(id(3))).revoked_by_root_id, id(1));
    EXPECT_EQ(grants.table.at(grants.index(id(3))).stamp.commit_seq, 13U);
    EXPECT_EQ(grants.table.at(grants.index(id(2))).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_TRUE(grants.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, nested_roots_have_independent_cursors_and_already_terminal_nodes_keep_stamps) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1)));
    ASSERT_TRUE(grants.add(3, id(2)));
    ASSERT_TRUE(grants.add(4, id(1), 10));
    grants.table.expire(3, {8, 8});
    grants.revoke(id(2), 10);
    grants.revoke(id(1), 11);
    uint64_t seq = 11;
    grants.clean(seq);
    EXPECT_EQ(grants.table.at(0).revoked_by_root_id, id(1));
    EXPECT_EQ(grants.table.at(1).revoked_by_root_id, id(2));
    EXPECT_EQ(grants.table.at(2).revoked_by_root_id, id(2));
    EXPECT_EQ(grants.table.at(1).stamp.commit_seq, 10U);
    EXPECT_EQ(grants.table.at(0).stamp.commit_seq, 11U);
    EXPECT_EQ(grants.table.at(3).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(grants.table.at(3).stamp.commit_seq, 8U);
    EXPECT_EQ(grants.table.at(3).revoked_by_root_id, 0U);
    EXPECT_TRUE(grants.table.consistent());
}

// C-AUTH-023/026: terminal ordering, durable cleanup and Gates govern safe replacement.
TEST(GrantRevoke, incomplete_cursor_gate_and_unconfirmed_progress_each_block_leaf_reclaim) {
    Grants grants(1);
    ASSERT_TRUE(grants.add(1));
    grants.revoke(id(1), 10);
    GrantTable::Admission admission{};
    const auto request = grants.input();
    EXPECT_EQ(grants.table.check_create(request, 0, ready, admission).status,
              AYNI_CAPACITY_EXCEEDED);
    GrantTable::CleanupPlan next{};
    ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
    EXPECT_FALSE(grants.table.apply_cleanup(next, {10, 10}));
    ASSERT_TRUE(grants.table.apply_cleanup(next, {11, 11}));
    EXPECT_EQ(grants.table.check_create(request, 0, {1, 10, 10}, admission).status,
              AYNI_CAPACITY_EXCEEDED);
    ASSERT_TRUE(grants.table.install_gate(id(1)));
    EXPECT_EQ(grants.table.check_create(request, 0, ready, admission).status,
              AYNI_CAPACITY_EXCEEDED);
    ASSERT_TRUE(grants.table.release_gate(id(1)));
    ASSERT_EQ(grants.table.check_create(request, 0, ready, admission).status, AYNI_OK);
    EXPECT_EQ(admission.replaced_grant_id, id(1));
    uint32_t index = 99;
    ASSERT_TRUE(grants.table.insert(request, admission, id(2), {12, 12}, index));
    EXPECT_EQ(index, 0U);
    EXPECT_EQ(grants.table.at(index).revoked_by_root_id, 0U);
    EXPECT_EQ(grants.table.at(index).cleanup_stamp.commit_seq, 0U);
    EXPECT_TRUE(grants.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, cleanup_plan_mismatches_and_stale_revoke_preserve_published_facts) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1)));
    grants.revoke(id(1), 10);
    GrantTable::CleanupPlan next{};
    ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
    for (uint32_t fault = 0; fault < 4; ++fault) {
        auto bad = next;
        if (fault == 0U) {
            bad.root_grant_id = id(2);
        } else if (fault == 1U) {
            bad.through_grant_id = id(3);
        } else if (fault == 2U) {
            bad.revoked_grant_id = id(2);
        }
        const TransitionStamp stamp =
            fault == 3U ? TransitionStamp{11, 0} : TransitionStamp{11, 11};
        EXPECT_FALSE(grants.table.apply_cleanup(bad, stamp));
        EXPECT_EQ(grants.table.at(0).cleanup_after_grant_id, 0U);
        EXPECT_EQ(grants.table.at(1).lifecycle, AYNI_GRANT_ACTIVE);
    }
    ASSERT_TRUE(grants.table.apply_cleanup(next, {11, 11}));
    const auto noop = grants.table.plan_accepted_revoke(id(1));
    EXPECT_FALSE(grants.table.apply_revoke(noop, {11, 11}));
    EXPECT_EQ(grants.table.at(0).stamp.commit_seq, 10U);
    EXPECT_EQ(grants.table.at(0).cleanup_stamp.commit_seq, 11U);
    grants.table.clear();
    EXPECT_FALSE(grants.table.apply_cleanup(next, {12, 12}));
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, cleanup_finishes_at_frozen_frontier_when_a_retained_unrelated_leaf_was_replaced) {
    Grants grants(3);
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1)));
    ASSERT_TRUE(grants.add(3, 0, 100));
    grants.table.expire(2, {8, 8});
    grants.revoke(id(1), 10);
    GrantTable::Admission admission{};
    const auto request = grants.input();
    ASSERT_EQ(grants.table.check_create(request, 0, ready, admission).status, AYNI_OK);
    ASSERT_EQ(admission.replaced_grant_id, id(3));
    uint32_t index = 0;
    ASSERT_TRUE(grants.table.insert(request, admission, id(4), {11, 11}, index));
    GrantTable::CleanupPlan next{};
    ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
    ASSERT_EQ(next.after_grant_id, id(1));
    ASSERT_TRUE(grants.table.apply_cleanup(next, {12, 12}));
    ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
    ASSERT_EQ(next.revoked_grant_id, id(2));
    ASSERT_TRUE(grants.table.apply_cleanup(next, {13, 13}));
    ASSERT_TRUE(grants.table.next_revocation_cleanup(next));
    EXPECT_EQ(next.after_grant_id, id(3));
    EXPECT_EQ(next.revoked_grant_id, 0U);
    ASSERT_TRUE(grants.table.apply_cleanup(next, {14, 14}));
    EXPECT_FALSE(grants.table.next_revocation_cleanup(next));
    EXPECT_EQ(grants.table.at(index).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_TRUE(grants.table.consistent());
}

// C-AUTH-024: accepted volatile control denies before persistence and survives reload.
TEST(GrantRevoke, gate_release_searches_past_other_live_targets) {
    Grants grants;
    ASSERT_TRUE(grants.table.install_gate(id(1)));
    ASSERT_TRUE(grants.table.install_gate(id(2)));
    EXPECT_FALSE(grants.table.release_gate(id(3)));
    EXPECT_TRUE(grants.table.release_gate(id(2)));
    EXPECT_TRUE(grants.table.gated(id(1)));
    EXPECT_EQ(grants.table.gate_count(), 1U);
}

// C-AUTH-023/026: terminal ordering, durable cleanup and Gates govern safe replacement.
TEST(GrantRevoke,
     durable_revoked_child_is_reclaimed_before_root_and_progress_does_not_reorder_root) {
    Grants grants(2);
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1)));
    grants.revoke(id(1), 10);
    uint64_t seq = 10;
    grants.clean(seq);
    GrantTable::Admission admission{};
    const auto request = grants.input();
    ASSERT_EQ(grants.table.check_create(request, 0, ready, admission).status, AYNI_OK);
    EXPECT_EQ(admission.replaced_grant_id, id(2));
    uint32_t index = 0;
    ASSERT_TRUE(grants.table.insert(request, admission, id(3), {20, 20}, index));
    ASSERT_EQ(grants.table.check_create(request, 0, ready, admission).status, AYNI_OK);
    EXPECT_EQ(admission.replaced_grant_id, id(1));
    EXPECT_TRUE(grants.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, journal_replays_root_then_atomic_descendant_state_and_progress) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    Grants restored;
    ASSERT_EQ(restored.table.restore_create(source.record(0), {1, 1}),
              GrantTable::Restore::restored);
    ASSERT_EQ(restored.table.restore_create(source.record(1), {2, 2}),
              GrantTable::Restore::restored);
    const GrantRevocationRecord root{id(1), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(2)};
    ASSERT_EQ(restored.table.restore_revocation(root, {10, 10}), GrantTable::Restore::restored);
    const GrantStateRecord child{id(2), 2, AYNI_GRANT_REVOKED, id(1)};
    EXPECT_EQ(restored.table.restore_state(child, {11, 11}), GrantTable::Restore::corrupt);
    ASSERT_EQ(restored.table.restore_progress({id(1), id(1), id(2)}, {11, 11}),
              GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.restore_progress({id(1), id(2), id(2)}, {12, 12}),
              GrantTable::Restore::corrupt);
    ASSERT_EQ(restored.table.restore_state(child, {12, 12}), GrantTable::Restore::restored);
    EXPECT_FALSE(restored.table.consistent()); // the batch must still publish its progress
    ASSERT_EQ(restored.table.restore_progress({id(1), id(2), id(2)}, {12, 12}),
              GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.restore_state(child, {13, 13}), GrantTable::Restore::corrupt);
    EXPECT_EQ(restored.table.restore_progress({id(1), id(2), id(2)}, {13, 13}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(restored.table.at(0).stamp.commit_seq, 10U);
    EXPECT_EQ(restored.table.at(0).cleanup_stamp.commit_seq, 12U);
    EXPECT_TRUE(restored.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, replay_rejects_forged_effect_watermark_revision_and_progress_without_mutation) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(3, id(1)));
    const GrantRevocationRecord valid{id(1), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(3)};
    for (uint32_t fault = 0; fault < 5; ++fault) {
        auto bad = valid;
        switch (fault) {
        case 0:
            bad.effect = AYNI_INTENT_EFFECT_NO_CHANGE;
            break;
        case 1:
            bad.revision = 1;
            break;
        case 2:
            bad.cleanup_after_grant_id = id(1);
            break;
        case 3:
            bad.cleanup_through_grant_id = id(2);
            break;
        default:
            bad.grant_id = id(9);
            break;
        }
        EXPECT_EQ(grants.table.restore_revocation(bad, {10, 10}), GrantTable::Restore::corrupt);
        EXPECT_EQ(grants.table.at(0).revision, 1U);
    }
    ASSERT_EQ(grants.table.restore_revocation(valid, {10, 10}), GrantTable::Restore::restored);
    for (const auto &bad : {GrantRevocationProgressRecord{id(9), id(1), id(3)},
                            GrantRevocationProgressRecord{id(1), 0, id(3)},
                            GrantRevocationProgressRecord{id(1), id(1), id(4)},
                            GrantRevocationProgressRecord{id(1), id(2), id(3)},
                            GrantRevocationProgressRecord{id(1), id(4), id(3)}}) {
        EXPECT_EQ(grants.table.restore_progress(bad, {11, 11}), GrantTable::Restore::corrupt);
        EXPECT_EQ(grants.table.at(0).cleanup_after_grant_id, 0U);
    }
    EXPECT_EQ(grants.table.restore_progress({id(1), id(1), id(3)}, {10, 10}),
              GrantTable::Restore::corrupt);
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, snapshot_keeps_terminal_and_cleanup_stamps_and_can_resume_after_partial_scan) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    ASSERT_TRUE(source.add(3, id(2)));
    source.revoke(id(1), 10);
    GrantTable::CleanupPlan step{};
    ASSERT_TRUE(source.table.next_revocation_cleanup(step));
    ASSERT_TRUE(source.table.apply_cleanup(step, {11, 11}));
    ASSERT_TRUE(source.table.next_revocation_cleanup(step));
    ASSERT_TRUE(source.table.apply_cleanup(step, {12, 12}));
    Grants restored;
    for (uint32_t row = 0; row < source.table.size(); ++row) {
        ASSERT_EQ(restored.table.restore_snapshot(source.snapshot(row)),
                  GrantTable::Restore::restored);
    }
    EXPECT_TRUE(restored.table.consistent());
    EXPECT_EQ(restored.table.at(0).stamp.commit_seq, 10U);
    EXPECT_EQ(restored.table.at(0).cleanup_stamp.commit_seq, 12U);
    EXPECT_EQ(restored.table.check_chain(2, ready).reason, AYNI_REASON_GRANT_REVOKED);
    ASSERT_TRUE(restored.table.next_revocation_cleanup(step));
    EXPECT_EQ(step.revoked_grant_id, id(3));
    EXPECT_TRUE(restored.table.apply_cleanup(step, {13, 13}));
    EXPECT_FALSE(restored.table.next_revocation_cleanup(step));
    EXPECT_TRUE(restored.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, snapshot_rejects_non_ancestor_revoker_and_malformed_cursor_or_stamp_atomically) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    source.revoke(id(1), 10);
    uint64_t sequence = 10;
    source.clean(sequence);
    const auto root = source.snapshot(0);
    for (uint32_t fault = 0; fault < 6; ++fault) {
        auto bad = root;
        switch (fault) {
        case 0:
            bad.revoked_by_root_id = 0;
            break;
        case 1:
            bad.cleanup_through_grant_id = 0;
            break;
        case 2:
            bad.cleanup_after_grant_id = id(3);
            break;
        case 3:
            bad.cleanup_stamp = {9, 9};
            break;
        case 4:
            bad.cleanup_stamp = {10, 11};
            break;
        default:
            bad.lifecycle = AYNI_GRANT_ACTIVE;
            bad.revision = 1;
            bad.stamp = bad.created_stamp;
            break;
        }
        Grants restored;
        EXPECT_EQ(restored.table.restore_snapshot(bad), GrantTable::Restore::corrupt);
        EXPECT_EQ(restored.table.size(), 0U);
    }
    Grants restored;
    ASSERT_EQ(restored.table.restore_snapshot(root), GrantTable::Restore::restored);
    auto child = source.snapshot(1);
    child.lifecycle = AYNI_GRANT_REVOKED;
    child.revision = 2;
    child.stamp = {12, 12};
    child.revoked_by_root_id = id(9);
    EXPECT_EQ(restored.table.restore_snapshot(child), GrantTable::Restore::corrupt);
    child.revoked_by_root_id = id(1);
    child.cleanup_after_grant_id = id(1);
    EXPECT_EQ(restored.table.restore_snapshot(child), GrantTable::Restore::corrupt);
    child.cleanup_after_grant_id = 0;
    ASSERT_EQ(restored.table.restore_snapshot(child), GrantTable::Restore::restored);
    EXPECT_TRUE(restored.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, snapshot_child_cannot_claim_cleanup_beyond_its_roots_cursor_or_stamp) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    source.revoke(id(1), 10);
    uint64_t sequence = 10;
    source.clean(sequence);
    const auto child = source.snapshot(1);
    auto root = source.snapshot(0);
    for (uint32_t fault = 0; fault < 2; ++fault) {
        auto bad_root = root;
        if (fault == 0U) {
            bad_root.cleanup_after_grant_id = id(1);
        } else {
            bad_root.cleanup_stamp = {11, 11};
        }
        Grants restored;
        ASSERT_EQ(restored.table.restore_snapshot(bad_root), GrantTable::Restore::restored);
        EXPECT_EQ(restored.table.restore_snapshot(child), GrantTable::Restore::corrupt);
        EXPECT_EQ(restored.table.size(), 1U);
    }
    Grants none(0);
    EXPECT_EQ(none.table.restore_snapshot(root), GrantTable::Restore::no_room);
    EXPECT_EQ(none.table.size(), 0U);
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, snapshot_frontier_covers_active_and_terminal_descendants_atomically) {
    for (uint32_t state = 0; state < 3; ++state) {
        Grants source;
        ASSERT_TRUE(source.add(1));
        ASSERT_TRUE(source.add(2, id(1), state == 1U ? 100U : 0U));
        if (state == 1U) {
            source.table.expire(1, {8, 8});
        } else if (state == 2U) {
            source.revoke(id(2), 8);
        }
        source.revoke(id(1), 10);
        const auto root = source.snapshot(0);
        const auto child = source.snapshot(1);
        ASSERT_EQ(root.cleanup_through_grant_id, id(2));

        Grants valid;
        ASSERT_EQ(valid.table.restore_snapshot(root), GrantTable::Restore::restored);
        ASSERT_EQ(valid.table.restore_snapshot(child), GrantTable::Restore::restored);
        EXPECT_TRUE(valid.table.consistent());

        auto shortened = root;
        shortened.cleanup_through_grant_id = id(1);
        Grants restored;
        ASSERT_EQ(restored.table.restore_snapshot(shortened), GrantTable::Restore::restored);
        EXPECT_EQ(restored.table.restore_snapshot(child), GrantTable::Restore::corrupt);
        EXPECT_EQ(restored.table.size(), 1U);
        EXPECT_EQ(restored.table.at(0).cleanup_through_grant_id, id(1));
        EXPECT_TRUE(restored.table.consistent());
    }
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, non_revoked_snapshots_reject_each_revocation_metadata_field_atomically) {
    for (const bool expired : {false, true}) {
        Grants source;
        ASSERT_TRUE(source.add(1, 0, expired ? 100U : 0U));
        if (expired) {
            source.table.expire(0, {8, 8});
        }
        const auto record = source.snapshot(0);
        for (uint32_t field = 0; field < 5; ++field) {
            auto bad = record;
            switch (field) {
            case 0:
                bad.revoked_by_root_id = id(1);
                break;
            case 1:
                bad.cleanup_after_grant_id = id(1);
                break;
            case 2:
                bad.cleanup_through_grant_id = id(1);
                break;
            case 3:
                bad.cleanup_stamp.transition_id = 1;
                break;
            default:
                bad.cleanup_stamp.commit_seq = 1;
                break;
            }
            Grants restored;
            EXPECT_EQ(restored.table.restore_snapshot(bad), GrantTable::Restore::corrupt);
            EXPECT_EQ(restored.table.size(), 0U);
        }
    }
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, descendant_snapshots_reject_private_cursor_fields_and_out_of_order_stamps) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    source.revoke(id(1), 10);
    uint64_t sequence = 10;
    source.clean(sequence);
    const auto child = source.snapshot(1);
    for (uint32_t field = 0; field < 7; ++field) {
        Grants restored;
        ASSERT_EQ(restored.table.restore_snapshot(source.snapshot(0)),
                  GrantTable::Restore::restored);
        auto bad = child;
        switch (field) {
        case 0:
            bad.cleanup_after_grant_id = id(1);
            break;
        case 1:
            bad.cleanup_through_grant_id = id(2);
            break;
        case 2:
            bad.cleanup_stamp.transition_id = 1;
            break;
        case 3:
            bad.cleanup_stamp.commit_seq = 1;
            break;
        case 4:
            bad.revoked_by_root_id = id(9);
            break;
        case 5:
            bad.stamp = {9, 9};
            break;
        default:
            bad.stamp = {13, 13};
            break;
        }
        EXPECT_EQ(restored.table.restore_snapshot(bad), GrantTable::Restore::corrupt);
        EXPECT_EQ(restored.table.size(), 1U);
        EXPECT_TRUE(restored.table.consistent());
    }
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, restored_child_state_requires_its_unfinished_self_root_and_ordered_progress) {
    Grants grants;
    ASSERT_TRUE(grants.add(1));
    ASSERT_TRUE(grants.add(2, id(1), 100));
    ASSERT_TRUE(grants.add(3));
    const GrantStateRecord valid{id(2), 2, AYNI_GRANT_REVOKED, id(1)};
    for (const uint64_t root : {id(9), id(2), id(3)}) {
        auto bad = valid;
        bad.revoked_by_root_id = root;
        EXPECT_EQ(grants.table.restore_state(bad, {10, 10}), GrantTable::Restore::corrupt);
        EXPECT_EQ(grants.table.at(1).lifecycle, AYNI_GRANT_ACTIVE);
    }
    const GrantStateRecord bad_expiry{id(2), 2, AYNI_GRANT_EXPIRED, id(1)};
    EXPECT_EQ(grants.table.restore_state(bad_expiry, {10, 10}), GrantTable::Restore::corrupt);
    grants.revoke(id(3), 10);
    auto unrelated = valid;
    unrelated.revoked_by_root_id = id(3);
    EXPECT_EQ(grants.table.restore_state(unrelated, {11, 11}), GrantTable::Restore::corrupt);
    grants.revoke(id(1), 11);
    EXPECT_EQ(grants.table.restore_state(valid, {11, 11}), GrantTable::Restore::corrupt);
    ASSERT_EQ(grants.table.restore_progress({id(1), id(1), id(3)}, {12, 12}),
              GrantTable::Restore::restored);
    EXPECT_EQ(grants.table.restore_state(valid, {12, 12}), GrantTable::Restore::corrupt);
    ASSERT_EQ(grants.table.restore_state(valid, {13, 13}), GrantTable::Restore::restored);
    ASSERT_EQ(grants.table.restore_progress({id(1), id(2), id(3)}, {13, 13}),
              GrantTable::Restore::restored);
    EXPECT_EQ(grants.table.restore_progress({id(1), id(2), id(3)}, {14, 14}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(grants.table.restore_progress({id(1), id(4), id(3)}, {14, 14}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(grants.table.restore_progress({id(1), id(3), id(3)}, {0, 14}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(grants.table.at(0).cleanup_after_grant_id, id(2));
    EXPECT_EQ(grants.table.at(1).stamp.commit_seq, 13U);
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, snapshot_frontier_covers_indirect_descendants_of_each_revoked_root) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    ASSERT_TRUE(source.add(3, id(2)));
    source.revoke(id(1), 10);
    auto shortened = source.snapshot(0);
    shortened.cleanup_through_grant_id = id(2);
    Grants restored;
    ASSERT_EQ(restored.table.restore_snapshot(shortened), GrantTable::Restore::restored);
    ASSERT_EQ(restored.table.restore_snapshot(source.snapshot(1)), GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.restore_snapshot(source.snapshot(2)), GrantTable::Restore::corrupt);
    EXPECT_EQ(restored.table.size(), 2U);
    EXPECT_TRUE(restored.table.consistent());

    Grants valid;
    for (uint32_t row = 0; row < source.table.size(); ++row) {
        ASSERT_EQ(valid.table.restore_snapshot(source.snapshot(row)),
                  GrantTable::Restore::restored);
    }
    EXPECT_TRUE(valid.table.consistent());
    uint64_t sequence = 10;
    valid.clean(sequence);
    EXPECT_EQ(valid.table.at(1).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(valid.table.at(2).lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_TRUE(valid.table.consistent());
}

// C-AUTH-023/026: terminal ordering, durable cleanup and Gates govern safe replacement.
TEST(GrantRevoke, replay_replacement_requires_completed_durable_order_but_does_not_drop_live_gate) {
    Grants source(1);
    ASSERT_TRUE(source.add(1));
    source.revoke(id(1), 10);
    auto replacement = source.record(0);
    replacement.grant_id = id(2);
    replacement.replaced_grant_id = id(1);
    EXPECT_EQ(source.table.restore_create(replacement, {11, 11}), GrantTable::Restore::corrupt);
    uint64_t seq = 10;
    source.clean(seq);
    const auto root = source.snapshot(0);
    Grants restored(1);
    ASSERT_TRUE(restored.table.install_gate(id(1)));
    ASSERT_EQ(restored.table.restore_snapshot(root), GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.restore_create(replacement, {11, 11}), GrantTable::Restore::corrupt);
    ASSERT_EQ(restored.table.restore_create(replacement, {12, 12}), GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.at(0).grant_id, id(2));
    EXPECT_TRUE(restored.table.gated(id(1)));
    const auto gone = restored.table.plan_accepted_revoke(id(1));
    EXPECT_EQ(gone.effect, AYNI_INTENT_EFFECT_TARGET_GONE);
    EXPECT_EQ(restored.table.restore_revocation({id(1), 0, AYNI_INTENT_EFFECT_TARGET_GONE, 0, 0},
                                                {13, 13}),
              GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.size(), 1U);
    EXPECT_TRUE(restored.table.consistent());
}

// C-AUTH-026: bounded ID-ordered cleanup and atomic recovery retain the original root fact.
TEST(GrantRevoke, restored_creation_cannot_cross_a_revoked_grandparent_with_an_active_parent) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    ASSERT_TRUE(source.add(2, id(1)));
    ASSERT_TRUE(source.add(3, id(2)));
    const auto child = source.record(2);
    source.revoke(id(1), 10);
    Grants restored;
    ASSERT_EQ(restored.table.restore_snapshot(source.snapshot(0)), GrantTable::Restore::restored);
    ASSERT_EQ(restored.table.restore_snapshot(source.snapshot(1)), GrantTable::Restore::restored);
    EXPECT_EQ(restored.table.at(1).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(restored.table.restore_create(child, {11, 11}), GrantTable::Restore::corrupt);
    const GrantSnapshotRecord late{child, {11, 11}, {11, 11}, 1, AYNI_GRANT_ACTIVE};
    EXPECT_EQ(restored.table.restore_snapshot(late), GrantTable::Restore::corrupt);
    EXPECT_EQ(restored.table.size(), 2U);
    const GrantSnapshotRecord old{child, {3, 3}, {3, 3}, 1, AYNI_GRANT_ACTIVE};
    EXPECT_EQ(restored.table.restore_snapshot(old), GrantTable::Restore::restored);
    EXPECT_TRUE(restored.table.consistent());
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, root_state_and_progress_match_independent_little_endian_literals) {
    constexpr std::array<uint8_t, 32> root{1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0};
    std::array<uint8_t, 32> bytes{};
    write_grant_revocation_record({id(1), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(3)}, bytes.data());
    EXPECT_EQ(bytes, root);
    GrantRevocationRecord decoded{};
    ASSERT_TRUE(
        read_grant_revocation_record(root.data(), static_cast<uint32_t>(root.size()), decoded));
    EXPECT_EQ(decoded.cleanup_through_grant_id, id(3));
    constexpr std::array<uint8_t, 24> state{2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0,
                                            3, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    Grant grant{};
    grant.grant_id = id(2);
    grant.revision = 2;
    grant.lifecycle = AYNI_GRANT_REVOKED;
    grant.revoked_by_root_id = id(1);
    EXPECT_EQ(grant_state_record_version_for(grant), 2U);
    EXPECT_EQ(grant_state_record_size_for(grant), 24U);
    std::array<uint8_t, 24> state_bytes{};
    write_grant_state_record(grant, state_bytes.data());
    EXPECT_EQ(state_bytes, state);
    GrantStateRecord child{};
    ASSERT_TRUE(read_grant_state_record(state.data(), 24, child, 2));
    EXPECT_EQ(child.revoked_by_root_id, id(1));
    EXPECT_FALSE(read_grant_state_record(state.data(), 24, child, 1));
    constexpr std::array<uint8_t, 24> progress{1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0,
                                               1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0};
    std::array<uint8_t, 24> progress_bytes{};
    write_grant_revocation_progress_record({id(1), id(2), id(3), 0}, progress_bytes.data());
    EXPECT_EQ(progress_bytes, progress);
    GrantRevocationProgressRecord cursor{};
    ASSERT_TRUE(read_grant_revocation_progress_record(progress.data(), 24, cursor));
    EXPECT_EQ(cursor.after_grant_id, id(2));
    EXPECT_EQ(cursor.through_grant_id, id(3));
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, malformed_versions_shapes_and_all_truncations_preserve_outputs) {
    std::array<uint8_t, 33> bytes{};
    write_grant_revocation_record({id(1), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(3)}, bytes.data());
    GrantRevocationRecord root{};
    root.grant_id = 91;
    for (uint32_t size = 0; size <= bytes.size(); ++size) {
        if (size != 32U) {
            EXPECT_FALSE(read_grant_revocation_record(bytes.data(), size, root));
            EXPECT_EQ(root.grant_id, 91U);
        }
    }
    EXPECT_FALSE(read_grant_revocation_record(nullptr, 32, root));
    for (const uint32_t offset : {0U, 4U, 8U, 12U, 16U, 24U, 28U}) {
        auto bad = bytes;
        bad[offset] = offset == 16U ? 1U : 0U;
        EXPECT_FALSE(read_grant_revocation_record(bad.data(), 32, root)) << offset;
    }
    for (const auto effect : {AYNI_INTENT_EFFECT_NO_CHANGE, AYNI_INTENT_EFFECT_TARGET_GONE}) {
        write_grant_revocation_record(
            {id(1), effect == AYNI_INTENT_EFFECT_NO_CHANGE ? 2U : 0U, effect, 0, 0}, bytes.data());
        ASSERT_TRUE(read_grant_revocation_record(bytes.data(), 32, root));
        EXPECT_EQ(root.effect, effect);
    }
    GrantStateRecord child{};
    child.grant_id = 91;
    EXPECT_FALSE(read_grant_state_record(bytes.data(), 24, child, 3));
    EXPECT_FALSE(read_grant_state_record(nullptr, 24, child, 2));
    EXPECT_EQ(child.grant_id, 91U);
    write_grant_revocation_progress_record({id(1), id(2), id(3), 0}, bytes.data());
    GrantRevocationProgressRecord cursor{};
    cursor.root_grant_id = 91;
    for (uint32_t size = 0; size <= bytes.size(); ++size) {
        if (size != 24U) {
            EXPECT_FALSE(read_grant_revocation_progress_record(bytes.data(), size, cursor));
            EXPECT_EQ(cursor.root_grant_id, 91U);
        }
    }
    EXPECT_FALSE(read_grant_revocation_progress_record(nullptr, 24, cursor));
    bytes[8] = 4;
    EXPECT_FALSE(read_grant_revocation_progress_record(bytes.data(), 24, cursor));
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, snapshot_v2_has_independent_eighty_byte_tail_and_v1_remains_explicit) {
    Grants source;
    ASSERT_TRUE(source.add(1, 0, 0, 10));
    ASSERT_TRUE(source.add(2, 0, 0, 11));
    ASSERT_TRUE(source.add(4, 0, 0, 12));
    source.revoke(id(1), 20);
    GrantTable::CleanupPlan step{};
    ASSERT_TRUE(source.table.next_revocation_cleanup(step));
    ASSERT_TRUE(source.table.apply_cleanup(step, {29, 29}));
    ASSERT_TRUE(source.table.next_revocation_cleanup(step));
    ASSERT_TRUE(source.table.apply_cleanup(step, {30, 30}));
    constexpr std::array<uint8_t, 80> tail{
        10, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0,  0, 0, 0, 0, 0, 20, 0, 0,  0, 0, 0, 0, 0, 20, 0, 0,
        0,  0, 0, 0, 0, 2, 0, 0, 0,  3, 0,  0, 0, 1, 0, 0, 0,  1, 0,  0, 0, 2, 0, 0, 0,  1, 0,
        0,  0, 4, 0, 0, 0, 1, 0, 0,  0, 30, 0, 0, 0, 0, 0, 0,  0, 30, 0, 0, 0, 0, 0, 0,  0};
    const Grant &root = source.table.at(0);
    EXPECT_EQ(grant_snapshot_record_size(root, 2), grant_record_size(root) + 80U);
    EXPECT_EQ(grant_snapshot_record_max(64), 320U);
    EXPECT_EQ(grant_snapshot_record_max(4096), 8384U);
    std::vector<uint8_t> bytes(grant_snapshot_record_size(root, 2));
    write_grant_snapshot_record(root, source.table.source_domain(0), source.table.scope(0),
                                bytes.data(), 2);
    EXPECT_EQ(std::vector<uint8_t>(bytes.end() - 80, bytes.end()),
              std::vector<uint8_t>(tail.begin(), tail.end()));
    GrantSnapshotRecord decoded{};
    ASSERT_TRUE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, 2));
    EXPECT_EQ(decoded.stamp.commit_seq, 20U);
    EXPECT_EQ(decoded.cleanup_stamp.commit_seq, 30U);
    EXPECT_EQ(decoded.cleanup_after_grant_id, id(2));
    EXPECT_FALSE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, 1));
    EXPECT_FALSE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, 3));
    EXPECT_EQ(grant_snapshot_record_size(root, 3), 0U);
    EXPECT_EQ(grant_snapshot_record_size(root, 1), 0U);
    const auto before = bytes;
    write_grant_snapshot_record(root, source.table.source_domain(0), source.table.scope(0),
                                bytes.data(), 1);
    EXPECT_EQ(bytes, before);
    write_grant_snapshot_record(root, source.table.source_domain(0), source.table.scope(0),
                                bytes.data(), 3);
    EXPECT_EQ(bytes, before);
    std::vector<uint8_t> old(grant_snapshot_record_size(source.table.at(1), 1));
    write_grant_snapshot_record(source.table.at(1), source.table.source_domain(1),
                                source.table.scope(1), old.data(), 1);
    ASSERT_TRUE(
        read_grant_snapshot_record(old.data(), static_cast<uint32_t>(old.size()), decoded, 1));
    EXPECT_EQ(decoded.revoked_by_root_id, 0U);
    EXPECT_EQ(decoded.cleanup_stamp.commit_seq, 0U);
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, revoked_state_rejects_each_identity_revision_and_lifecycle_boundary) {
    Grant grant{};
    grant.grant_id = id(2);
    grant.revision = 2;
    grant.lifecycle = AYNI_GRANT_REVOKED;
    grant.revoked_by_root_id = id(1);
    std::array<uint8_t, 24> valid{};
    write_grant_state_record(grant, valid.data());
    GrantStateRecord decoded{};
    ASSERT_TRUE(read_grant_state_record(valid.data(), 24, decoded, 2));
    for (uint32_t fault = 0; fault < 8; ++fault) {
        auto bad = valid;
        switch (fault) {
        case 0:
            bad[0] = 0; // zero counter
            break;
        case 1:
            bad[4] = 0; // zero epoch
            break;
        case 2:
            bad[8] = 1; // nonterminal revision
            break;
        case 3:
            bad[12] = static_cast<uint8_t>(AYNI_GRANT_ACTIVE);
            break;
        case 4:
            bad[12] = static_cast<uint8_t>(AYNI_GRANT_EXPIRED);
            break;
        case 5:
            bad[16] = 0; // revoker counter
            break;
        case 6:
            bad[20] = 0; // revoker epoch
            break;
        default:
            bad[16] = 2; // root revocation belongs to type15, never type14
            break;
        }
        decoded = {91, 92, 93, 94};
        EXPECT_FALSE(read_grant_state_record(bad.data(), 24, decoded, 2));
        EXPECT_EQ(decoded.grant_id, 91U);
        EXPECT_EQ(decoded.revision, 92U);
        EXPECT_EQ(decoded.lifecycle, 93U);
        EXPECT_EQ(decoded.revoked_by_root_id, 94U);
    }
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, root_outcomes_validate_revision_frontier_and_known_effect_atomically) {
    const std::array<GrantTable::RevokePlan, 9> malformed{{
        {id(0), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(3)},
        {1, 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(3)},
        {id(1), 2, AYNI_INTENT_EFFECT_CHANGED, id(1), id(3)},
        {id(2), 2, AYNI_INTENT_EFFECT_CHANGED, 0, id(1)},
        {id(1), 1, AYNI_INTENT_EFFECT_NO_CHANGE, 0, 0},
        {id(1), 0, AYNI_INTENT_EFFECT_NO_CHANGE, 0, 0},
        {id(1), 2, AYNI_INTENT_EFFECT_TARGET_GONE, 0, 0},
        {id(1), 0, static_cast<ayni_intent_effect_t>(999), 0, 0},
        {id(1), 2, AYNI_INTENT_EFFECT_NO_CHANGE, 0, id(3)},
    }};
    for (const auto &plan : malformed) {
        std::array<uint8_t, 32> bytes{};
        write_grant_revocation_record(plan, bytes.data());
        GrantRevocationRecord decoded{91, 92, 93, 94, 95};
        EXPECT_FALSE(read_grant_revocation_record(bytes.data(), 32, decoded));
        EXPECT_EQ(decoded.grant_id, 91U);
        EXPECT_EQ(decoded.revision, 92U);
        EXPECT_EQ(decoded.effect, 93U);
        EXPECT_EQ(decoded.cleanup_after_grant_id, 94U);
        EXPECT_EQ(decoded.cleanup_through_grant_id, 95U);
    }
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, progress_rejects_invalid_ids_and_both_ordering_boundaries_atomically) {
    const std::array<GrantTable::CleanupPlan, 8> malformed{{
        {id(0), id(2), id(3), 0},
        {1, id(2), id(3), 0},
        {id(1), id(0), id(3), 0},
        {id(1), 2, id(3), 0},
        {id(1), id(2), id(0), 0},
        {id(1), id(2), 3, 0},
        {id(1), id(4), id(3), 0},
        {id(2), id(1), id(1), 0},
    }};
    for (const auto &plan : malformed) {
        std::array<uint8_t, 24> bytes{};
        write_grant_revocation_progress_record(plan, bytes.data());
        GrantRevocationProgressRecord decoded{91, 92, 93};
        EXPECT_FALSE(read_grant_revocation_progress_record(bytes.data(), 24, decoded));
        EXPECT_EQ(decoded.root_grant_id, 91U);
        EXPECT_EQ(decoded.after_grant_id, 92U);
        EXPECT_EQ(decoded.through_grant_id, 93U);
    }
}

// ADR-0031 / C-AUTH-026: versioned revocation record shape and failure outputs.
TEST(GrantRevokeRecords, legacy_snapshot_cannot_encode_revoked_lifecycle_in_its_old_tail) {
    Grants source;
    ASSERT_TRUE(source.add(1));
    const Grant &grant = source.table.at(0);
    std::vector<uint8_t> bytes(grant_snapshot_record_size(grant, 1));
    write_grant_snapshot_record(grant, source.table.source_domain(0), source.table.scope(0),
                                bytes.data(), 1);
    GrantSnapshotRecord decoded{};
    ASSERT_TRUE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, 1));
    bytes[bytes.size() - 4U] = static_cast<uint8_t>(AYNI_GRANT_REVOKED);
    decoded.create.grant_id = 91;
    EXPECT_FALSE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, 1));
    EXPECT_EQ(decoded.create.grant_id, 91U);
}
} // namespace
