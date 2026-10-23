#include "authority/grant/grant_records.h"
#include "authority/grant/grant_table.h"
#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"
#include "foundation/little_endian.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using namespace ayni::authority;
constexpr uint64_t id(uint32_t sequence, uint64_t boot = 1) {
    return (boot << 32U) | sequence;
}
constexpr GrantContext ready{1, 100, 1000};

struct Input {
    std::array<uint8_t, 256> domain{};
    uint32_t domain_size = 0;
    ayni_scope_field_v1 field{};
    ayni_scope_v1 scope{};
    Input(int64_t minimum = -10, int64_t maximum = 10) {
        ayni_authority_value_set_v1 values{};
        values.struct_size = sizeof(values);
        values.kind = AYNI_AUTHORITY_I64;
        values.unit = {"count", 5};
        values.values.integer = {-10, 10};
        const ayni_authority_field_desc_v1 declaration{sizeof(declaration), 1,
                                                       AYNI_AUTHORITY_PARAMETER, 0, values};
        EXPECT_EQ(ayni::catalog::encode_domain(&declaration, 1, domain.data(),
                                               static_cast<uint32_t>(domain.size()), domain_size),
                  AYNI_OK);
        values.values.integer = {minimum, maximum};
        field = {sizeof(field), 1, values};
        scope = {sizeof(scope), 1, &field};
    }
    NewGrant request(uint64_t parent = 0, uint32_t flags = 0, uint64_t duration = 0) const {
        ayni::catalog::CatalogId capability;
        EXPECT_TRUE(capability.assign({"test.cap", 8}));
        return {id(100),
                parent,
                7,
                3,
                capability,
                1,
                {domain.data(), domain_size},
                &scope,
                duration == 0U ? AYNI_GRANT_PERMANENT : AYNI_GRANT_CURRENT_BOOT,
                duration,
                flags};
    }
};

class Grants {
  public:
    explicit Grants(uint32_t capacity = 8, uint32_t depth = 4, uint32_t bytes = 256)
        : memory_(GrantTable::storage_size({capacity, bytes, depth}) / sizeof(uint64_t) + 1U),
          table_(memory_.data(), {capacity, bytes, depth}) {}
    GrantTable *operator->() { return &table_; }
    GrantCheck add(const NewGrant &grant, uint32_t sequence, uint64_t stamp,
                   GrantContext context = ready, uint32_t expected = 0) {
        GrantTable::Admission admission{};
        const auto check = table_.check_create(grant, expected, context, admission);
        if (check.status == AYNI_OK) {
            EXPECT_TRUE(table_.insert(grant, admission, id(sequence, context.boot_epoch),
                                      {stamp, stamp}, last_));
        }
        return check;
    }
    uint32_t last() const { return last_; }

  private:
    std::vector<uint64_t> memory_;
    GrantTable table_;
    uint32_t last_ = 0;
};

GrantRecord record_of(GrantTable &table, uint32_t index) {
    const auto &grant = table.at(index);
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

TEST(GrantTable, checked_admission_is_read_only_and_insert_owns_both_canonical_bodies) {
    Input input(-2, 2);
    Grants table;
    auto grant = input.request();
    GrantTable::Admission admission{};
    ASSERT_EQ(table->check_create(grant, 0, ready, admission).status, AYNI_OK);
    EXPECT_EQ(table->size(), 0U);
    EXPECT_EQ(admission.depth, 0U);
    uint32_t index = 91;
    ASSERT_TRUE(table->insert(grant, admission, id(1), {10, 10}, index));
    const auto domain = table->source_domain(index);
    const std::vector<uint8_t> domain_before(domain.data, domain.data + domain.size);
    const auto scope = table->scope(index);
    const std::vector<uint8_t> scope_before(scope.data, scope.data + scope.size);
    input.domain[0] = 99;
    input.field.allowed.values.integer = {9, 10};
    EXPECT_EQ((std::vector<uint8_t>{domain.data, domain.data + domain.size}), domain_before);
    EXPECT_EQ((std::vector<uint8_t>{scope.data, scope.data + scope.size}), scope_before);
    EXPECT_EQ(table->at(index).deadline_us, 0U);
    EXPECT_EQ(table->check_chain(index, ready).status, AYNI_OK);
    EXPECT_TRUE(table->consistent());
}

TEST(GrantTable, malformed_flags_lifetimes_revisions_and_overflow_never_publish_a_plan) {
    Input input;
    Grants table;
    for (unsigned fault = 0; fault < 6; ++fault) {
        auto grant = input.request();
        uint32_t expected = 0;
        switch (fault) {
        case 0:
            grant.flags = 8;
            break;
        case 1:
            grant.lifetime = 0;
            break;
        case 2:
            grant.duration_us = 1;
            break;
        case 3:
            grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
            break;
        case 4:
            expected = 1;
            break;
        default:
            grant.subject_instance_id = 0;
            break;
        }
        GrantTable::Admission plan{91, 92, 93, 94, 95, 96};
        const auto check = table->check_create(grant, expected, ready, plan);
        EXPECT_EQ(check.status, AYNI_INVALID_ARGUMENT) << fault;
        EXPECT_EQ(check.reason, AYNI_REASON_GRANT_INPUT);
        EXPECT_EQ(plan.index, 91U);
        EXPECT_EQ(plan.boot_epoch, 96U);
        EXPECT_EQ(table->size(), 0U);
    }
    auto finite = input.request(0, 0, 6);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(finite, 0, {1, UINT64_MAX - 5U, 1000}, plan).status,
              AYNI_INVALID_ARGUMENT);
    finite.duration_us = 5;
    EXPECT_EQ(table->check_create(finite, 0, {1, UINT64_MAX - 5U, 1000}, plan).status, AYNI_OK);
    EXPECT_EQ(plan.deadline_us, UINT64_MAX);
}

TEST(GrantTable, known_scene_and_quota_requirements_are_explicitly_unsupported) {
    Input input;
    Grants table;
    for (const uint32_t flag : {AYNI_GRANT_REQUIRES_SCENE, AYNI_GRANT_REQUIRES_QUOTA}) {
        GrantTable::Admission plan{};
        const auto check = table->check_create(input.request(0, flag), 0, ready, plan);
        EXPECT_EQ(check.status, AYNI_UNSUPPORTED);
        EXPECT_EQ(check.reason, AYNI_REASON_GRANT_UNSUPPORTED);
        EXPECT_EQ(table->size(), 0U);
    }
}

TEST(GrantTable, parent_must_exist_match_revision_be_durable_and_allow_delegation) {
    Input input;
    Grants table;
    ASSERT_EQ(table.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 10).status, AYNI_OK);
    GrantTable::Admission plan{};
    auto child = input.request(id(9));
    EXPECT_EQ(table->check_create(child, 1, ready, plan).reason, AYNI_REASON_GRANT_UNKNOWN);
    child.parent_grant_id = id(1);
    EXPECT_EQ(table->check_create(child, 2, ready, plan).status, AYNI_REVISION_CONFLICT);
    const auto pending = table->check_create(child, 1, {1, 100, 9}, plan);
    EXPECT_EQ(pending.status, AYNI_BUSY);
    EXPECT_EQ(pending.reason, AYNI_REASON_GRANT_NOT_DURABLE);
    ASSERT_EQ(table.add(input.request(), 2, 11).status, AYNI_OK);
    child.parent_grant_id = id(2);
    EXPECT_EQ(table->check_create(child, 1, ready, plan).reason, AYNI_REASON_GRANT_PARENT);
}

TEST(GrantTable, delegation_checks_one_complete_parent_scope_and_exact_capability_domain) {
    Input parent(-5, 5);
    Input child(-4, 4);
    Input too_wide(-6, 4);
    Grants table;
    ASSERT_EQ(table.add(parent.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 10).status, AYNI_OK);
    ASSERT_EQ(table.add(child.request(id(1)), 2, 11, ready, 1).status, AYNI_OK);
    EXPECT_EQ(table->at(table.last()).depth, 1U);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(too_wide.request(id(1)), 1, ready, plan).reason,
              AYNI_REASON_GRANT_SCOPE);
    auto omitted = child.request(id(1));
    const ayni_scope_v1 full_domain{sizeof(full_domain), 0, nullptr};
    omitted.scope = &full_domain;
    EXPECT_EQ(table->check_create(omitted, 1, ready, plan).reason, AYNI_REASON_GRANT_SCOPE);
    auto different = child.request(id(1));
    ++different.capability_version;
    EXPECT_EQ(table->check_create(different, 1, ready, plan).reason, AYNI_REASON_GRANT_PARENT);
    different = child.request(id(1));
    ++different.owner_binding_revision;
    EXPECT_EQ(table->check_create(different, 1, ready, plan).reason, AYNI_REASON_GRANT_PARENT);
    Input changed;
    changed.domain[changed.domain_size - 8U] = 11; // same version, broader registered maximum
    EXPECT_EQ(table->check_create(changed.request(id(1)), 1, ready, plan).reason,
              AYNI_REASON_GRANT_PARENT);
}

TEST(GrantTable, every_ancestor_hold_and_the_edge_depth_limit_apply_to_descendants) {
    Input input;
    Grants table(8, 2);
    ASSERT_EQ(table.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 10).status, AYNI_OK);
    ASSERT_EQ(table.add(input.request(id(1), AYNI_GRANT_ALLOW_DELEGATION), 2, 11, ready, 1).status,
              AYNI_OK);
    ASSERT_EQ(table.add(input.request(id(2), AYNI_GRANT_ALLOW_DELEGATION), 3, 12, ready, 1).status,
              AYNI_OK);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(input.request(id(3)), 1, ready, plan).reason,
              AYNI_REASON_GRANT_DEPTH);
    table->hold_restored(0, AYNI_REASON_GRANT_SUBJECT);
    EXPECT_EQ(table->check_chain(2, ready).reason, AYNI_REASON_GRANT_SUBJECT);
    EXPECT_EQ(table->check_create(input.request(id(3)), 1, ready, plan).reason,
              AYNI_REASON_GRANT_SUBJECT);
    EXPECT_TRUE(table->consistent());
}

TEST(GrantTable, finite_child_deadline_cannot_outlive_its_parent_remaining_time) {
    Input input;
    Grants table;
    ASSERT_EQ(table.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION, 100), 1, 10).status, AYNI_OK);
    GrantTable::Admission plan{};
    const GrantContext later{1, 150, 1000};
    EXPECT_EQ(table->check_create(input.request(id(1)), 1, later, plan).reason,
              AYNI_REASON_GRANT_PARENT);
    EXPECT_EQ(table->check_create(input.request(id(1), 0, 51), 1, later, plan).reason,
              AYNI_REASON_GRANT_PARENT);
    ASSERT_EQ(table.add(input.request(id(1), 0, 50), 2, 11, later, 1).status, AYNI_OK);
    EXPECT_EQ(table->at(1).deadline_us, 200U);
    EXPECT_EQ(table->check_chain(1, {1, 199, 1000}).status, AYNI_OK);
    EXPECT_EQ(table->check_chain(1, {1, 200, 1000}).reason, AYNI_REASON_GRANT_EXPIRED);
    EXPECT_EQ(table->check_chain(0, {2, 0, 1000}).reason, AYNI_REASON_GRANT_EXPIRED);
}

TEST(GrantTable, finite_delegation_matches_an_independent_discrete_time_coverage_oracle) {
    Input input;
    Grants table;
    ASSERT_EQ(
        table.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION, 5), 1, 1, {1, 10, 1000}).status,
        AYNI_OK);
    for (uint64_t start = 10; start <= 16; ++start) {
        for (uint64_t duration = 1; duration <= 7; ++duration) {
            bool all_instants_covered = true;
            for (uint64_t instant = start; instant < start + duration; ++instant) {
                all_instants_covered = all_instants_covered && instant >= 10U && instant < 15U;
            }
            GrantTable::Admission plan{};
            const auto check =
                table->check_create(input.request(id(1), 0, duration), 1, {1, start, 1000}, plan);
            EXPECT_EQ(check.status == AYNI_OK, all_instants_covered)
                << "start=" << start << " duration=" << duration;
        }
    }
    EXPECT_EQ(table->size(), 1U);
}

TEST(GrantTable, expiry_is_immediate_but_only_a_durable_terminal_can_be_reclaimed) {
    Input input;
    Grants table(1);
    ASSERT_EQ(table.add(input.request(0, 0, 10), 1, 10).status, AYNI_OK);
    EXPECT_EQ(table->check_chain(0, {1, 110, 1000}).reason, AYNI_REASON_GRANT_EXPIRED);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(input.request(), 0, {1, 110, 1000}, plan).reason,
              AYNI_REASON_GRANT_CAPACITY);
    table->expire(0, {11, 11});
    EXPECT_EQ(table->check_create(input.request(), 0, {1, 110, 10}, plan).reason,
              AYNI_REASON_GRANT_CAPACITY);
    ASSERT_EQ(table->check_create(input.request(), 0, {1, 110, 11}, plan).status, AYNI_OK);
    EXPECT_EQ(plan.replaced_grant_id, id(1));
}

TEST(GrantTable, replacement_uses_terminal_order_then_id_instead_of_creation_order) {
    Input input;
    Grants table(2);
    ASSERT_EQ(table.add(input.request(0, 0, 10), 1, 1).status, AYNI_OK);
    ASSERT_EQ(table.add(input.request(0, 0, 10), 2, 2).status, AYNI_OK);
    table->expire(1, {10, 10});
    table->expire(0, {20, 20});
    GrantTable::Admission plan{};
    ASSERT_EQ(table->check_create(input.request(), 0, ready, plan).status, AYNI_OK);
    EXPECT_EQ(plan.replaced_grant_id, id(2));
    Grants tied(2);
    ASSERT_EQ(tied.add(input.request(0, 0, 10), 2, 1).status, AYNI_OK);
    ASSERT_EQ(tied.add(input.request(0, 0, 10), 1, 2).status, AYNI_OK);
    tied->expire_old_boot(2, {20, 20});
    ASSERT_EQ(tied->check_create(input.request(), 0, ready, plan).status, AYNI_OK);
    EXPECT_EQ(plan.replaced_grant_id, id(1));
}

TEST(GrantTable, retained_children_prevent_parent_replacement_until_the_leaf_has_been_replaced) {
    Input input;
    Grants table(2);
    ASSERT_EQ(table.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION, 10), 1, 1).status, AYNI_OK);
    ASSERT_EQ(table.add(input.request(id(1), 0, 10), 2, 2, ready, 1).status, AYNI_OK);
    table->expire(0, {10, 10});
    table->expire(1, {11, 11});
    ASSERT_EQ(table.add(input.request(), 3, 20).status, AYNI_OK);
    EXPECT_EQ(table->at(table.last()).replaced_grant_id, id(2));
    ASSERT_EQ(table.add(input.request(), 4, 21).status, AYNI_OK);
    EXPECT_EQ(table->at(table.last()).replaced_grant_id, id(1));
    EXPECT_TRUE(table->consistent());
}

// C-ID-010 / ADR-0030: CURRENT_BOOT authority belongs to exactly one boot,
// even when a caller presents a context older than the retained Grant.
TEST(GrantTable, finite_grants_authorize_only_their_own_boot) {
    Input input;
    Grants grants;
    const GrantContext created_in{2, 100, 1000};
    ASSERT_EQ(grants.add(input.request(0, 0, 100), 1, 10, created_in).status, AYNI_OK);
    ASSERT_EQ(grants->check_chain(grants.last(), created_in).status, AYNI_OK);
    for (const uint64_t boot : {1U, 3U}) {
        const auto check = grants->check_chain(grants.last(), {boot, 100, 1000});
        EXPECT_EQ(check.status, AYNI_DENIED) << boot;
        EXPECT_EQ(check.reason, AYNI_REASON_GRANT_EXPIRED) << boot;
    }
    EXPECT_EQ(grants->check_chain(grants.last(), created_in).status, AYNI_OK);
}

TEST(GrantTable, next_expiry_is_ordered_and_restored_finite_grants_never_reanchor) {
    Input input;
    Grants original;
    ASSERT_EQ(original.add(input.request(0, 0, 30), 1, 1).status, AYNI_OK);
    ASSERT_EQ(original.add(input.request(0, 0, 20), 2, 2).status, AYNI_OK);
    uint32_t index = 91;
    uint64_t deadline = 92;
    ASSERT_TRUE(original->next_expiry(ready, index, deadline));
    EXPECT_EQ(index, 1U);
    EXPECT_EQ(deadline, 120U);
    Grants restored;
    ASSERT_EQ(restored->restore_create(record_of(*original.operator->(), 0), {1, 1}),
              GrantTable::Restore::restored);
    EXPECT_EQ(restored->at(0).deadline_us, 0U);
    EXPECT_EQ(restored->check_chain(0, ready).reason, AYNI_REASON_GRANT_EXPIRED);
    EXPECT_TRUE(restored->has_old_boot_expiry(2));
    ASSERT_TRUE(restored->next_expiry({2, 9000, 1000}, index, deadline));
    EXPECT_EQ(deadline, 0U);
    restored->expire_old_boot(2, {10, 10});
    EXPECT_EQ(restored->at(0).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(restored->at(0).revision, 2U);
    EXPECT_FALSE(restored->has_old_boot_expiry(2));
    EXPECT_FALSE(restored->next_expiry(ready, index, deadline));
}

TEST(GrantTable, restore_rejects_orphan_expansion_cycles_and_wrong_expiry_states) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 1).status, AYNI_OK);
    ASSERT_EQ(source.add(input.request(id(1)), 2, 2, ready, 1).status, AYNI_OK);
    Grants restored;
    EXPECT_EQ(restored->restore_create(record_of(*source.operator->(), 1), {2, 2}),
              GrantTable::Restore::corrupt);
    ASSERT_EQ(restored->restore_create(record_of(*source.operator->(), 0), {1, 1}),
              GrantTable::Restore::restored);
    auto child = record_of(*source.operator->(), 1);
    child.grant.parent_grant_id = child.grant_id;
    EXPECT_EQ(restored->restore_create(child, {2, 2}), GrantTable::Restore::corrupt);
    EXPECT_EQ(restored->restore_state({id(1), 2, AYNI_GRANT_EXPIRED}, {3, 3}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(restored->size(), 1U);
    EXPECT_TRUE(restored->consistent());
}

TEST(GrantTable, exact_replacement_replays_without_reselecting_another_safe_candidate) {
    Input input;
    Grants source(2);
    ASSERT_EQ(source.add(input.request(0, 0, 10), 1, 1).status, AYNI_OK);
    ASSERT_EQ(source.add(input.request(0, 0, 10), 2, 2).status, AYNI_OK);
    const auto first = record_of(*source.operator->(), 0);
    const auto second = record_of(*source.operator->(), 1);
    Grants replay(2);
    ASSERT_EQ(replay->restore_create(first, {1, 1}), GrantTable::Restore::restored);
    ASSERT_EQ(replay->restore_create(second, {2, 2}), GrantTable::Restore::restored);
    ASSERT_EQ(replay->restore_state({id(1), 2, AYNI_GRANT_EXPIRED}, {20, 20}),
              GrantTable::Restore::restored);
    ASSERT_EQ(replay->restore_state({id(2), 2, AYNI_GRANT_EXPIRED}, {10, 10}),
              GrantTable::Restore::restored);
    auto replacement = first;
    replacement.grant_id = id(3);
    replacement.replaced_grant_id = id(1); // deliberately not the earliest safe leaf
    replacement.grant.lifetime = AYNI_GRANT_PERMANENT;
    replacement.grant.duration_us = 0;
    ASSERT_EQ(replay->restore_create(replacement, {30, 30}), GrantTable::Restore::restored);
    uint32_t index = 0;
    EXPECT_FALSE(replay->find(id(1), index));
    EXPECT_TRUE(replay->find(id(2), index));
    EXPECT_TRUE(replay->find(id(3), index));
}

TEST(GrantTable, snapshot_keeps_original_stamps_and_allows_an_already_removed_predecessor) {
    Input input;
    Grants source(1);
    ASSERT_EQ(source.add(input.request(0, 0, 10), 1, 1).status, AYNI_OK);
    source->expire(0, {2, 2});
    ASSERT_EQ(source.add(input.request(), 2, 3).status, AYNI_OK);
    const GrantSnapshotRecord snapshot{
        record_of(*source.operator->(), 0), {3, 3}, {3, 3}, 1, AYNI_GRANT_ACTIVE};
    Grants restored(1);
    ASSERT_EQ(restored->restore_snapshot(snapshot), GrantTable::Restore::restored);
    EXPECT_EQ(restored->at(0).replaced_grant_id, id(1));
    EXPECT_EQ(restored->at(0).created_stamp.commit_seq, 3U);
    EXPECT_EQ(restored->check_chain(0, ready).status, AYNI_OK);
    auto invalid = snapshot;
    invalid.revision = 2;
    Grants other;
    EXPECT_EQ(other->restore_snapshot(invalid), GrantTable::Restore::corrupt);
}

TEST(GrantTable,
     restore_distinguishes_configured_capacity_from_corrupt_facts_without_partial_insert) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 1).status, AYNI_OK);
    ASSERT_EQ(source.add(input.request(id(1)), 2, 2, ready, 1).status, AYNI_OK);
    const auto parent = record_of(*source.operator->(), 0);
    const auto child = record_of(*source.operator->(), 1);
    Grants empty(0);
    EXPECT_EQ(empty->restore_create(parent, {1, 1}), GrantTable::Restore::no_room);
    EXPECT_EQ(empty->size(), 0U);
    Grants undersized(1, 4, 4);
    EXPECT_EQ(undersized->restore_create(parent, {1, 1}), GrantTable::Restore::no_room);
    EXPECT_EQ(undersized->size(), 0U);
    Grants shallow(2, 0);
    ASSERT_EQ(shallow->restore_create(parent, {1, 1}), GrantTable::Restore::restored);
    EXPECT_EQ(shallow->restore_create(child, {2, 2}), GrantTable::Restore::no_room);
    EXPECT_EQ(shallow->size(), 1U);
    auto broken = parent;
    broken.scope.size = 1;
    EXPECT_EQ(empty->restore_create(broken, {1, 1}), GrantTable::Restore::corrupt);
    EXPECT_EQ(empty->size(), 0U);
}

TEST(GrantTable, snapshot_cannot_reintroduce_a_replaced_predecessor_in_either_record_order) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(), 1, 1).status, AYNI_OK);
    ASSERT_EQ(source.add(input.request(), 2, 2).status, AYNI_OK);
    const GrantSnapshotRecord first{
        record_of(*source.operator->(), 0), {1, 1}, {1, 1}, 1, AYNI_GRANT_ACTIVE};
    auto second = GrantSnapshotRecord{
        record_of(*source.operator->(), 1), {2, 2}, {2, 2}, 1, AYNI_GRANT_ACTIVE};
    second.create.replaced_grant_id = id(1);
    Grants predecessor_first;
    ASSERT_EQ(predecessor_first->restore_snapshot(first), GrantTable::Restore::restored);
    EXPECT_EQ(predecessor_first->restore_snapshot(second), GrantTable::Restore::corrupt);
    Grants replacement_first;
    ASSERT_EQ(replacement_first->restore_snapshot(second), GrantTable::Restore::restored);
    ASSERT_EQ(replacement_first->restore_snapshot(first), GrantTable::Restore::restored);
    EXPECT_FALSE(replacement_first->consistent());
}

TEST(GrantTable, expiry_rejects_zero_or_stale_stamps_and_permanent_grants_do_not_expire) {
    Input input;
    Grants table;
    ASSERT_EQ(table.add(input.request(0, 0, 10), 1, 10).status, AYNI_OK);
    ASSERT_EQ(table.add(input.request(), 2, 11).status, AYNI_OK);
    for (const ayni::foundation::TransitionStamp stamp :
         {ayni::foundation::TransitionStamp{0, 12}, {12, 0}, {10, 12}, {12, 10}}) {
        table->expire(0, stamp);
        EXPECT_EQ(table->at(0).revision, 1U);
    }
    table->expire(1, {12, 12});
    EXPECT_EQ(table->at(1).revision, 1U);
    table->expire_old_boot(2, {10, 12});
    EXPECT_EQ(table->at(0).revision, 1U);
    table->expire_old_boot(2, {12, 12});
    EXPECT_EQ(table->at(0).revision, 2U);
    EXPECT_EQ(table->at(0).stamp.commit_seq, 12U);
    table->expire(0, {13, 13});
    EXPECT_EQ(table->at(0).stamp.commit_seq, 12U);
    EXPECT_EQ(table->at(1).revision, 1U);
}

TEST(GrantTable, absent_or_changed_catalog_only_holds_persistent_facts_and_can_be_resolved_again) {
    Input input(-2, 2);
    Grants table(1);
    ASSERT_EQ(table.add(input.request(), 1, 1).status, AYNI_OK);
    std::vector<uint64_t> memory(
        ayni::catalog::CapabilityRegistry::storage_size(1, 256) / sizeof(uint64_t) + 1U);
    ayni::catalog::CapabilityRegistry missing(memory.data(), 1, 256);
    table->resolve_catalog(missing);
    EXPECT_EQ(table->check_chain(0, ready).reason, AYNI_REASON_CAPABILITY_UNKNOWN);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(input.request(), 0, ready, plan).reason,
              AYNI_REASON_GRANT_CAPACITY);
    auto values = input.field.allowed;
    values.values.integer = {-10, 11};
    ayni_authority_field_desc_v1 declaration{sizeof(declaration), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                             values};
    const ayni_capability_desc_v1 desc{sizeof(desc), 1, {"test.cap", 8}, 1, 0, &declaration};
    ASSERT_EQ(missing.add(desc), AYNI_OK);
    table->resolve_catalog(missing);
    EXPECT_EQ(table->check_chain(0, ready).reason, AYNI_REASON_GRANT_CATALOG_CHANGED);
    EXPECT_EQ(table->at(0).lifecycle, AYNI_GRANT_ACTIVE);
    EXPECT_EQ(table->at(0).revision, 1U);
    EXPECT_EQ(table->at(0).stamp.commit_seq, 1U);
    std::vector<uint64_t> original_memory(memory.size());
    ayni::catalog::CapabilityRegistry original(original_memory.data(), 1, 256);
    declaration.domain.values.integer = {-10, 10};
    ASSERT_EQ(original.add(desc), AYNI_OK);
    table->resolve_catalog(original);
    EXPECT_EQ(table->check_chain(0, ready).status, AYNI_OK);
}

TEST(GrantTable, invalid_identity_context_and_nested_scopes_leave_admission_unpublished) {
    Input input;
    Grants table;
    for (uint32_t fault = 0; fault < 10; ++fault) {
        auto grant = input.request();
        auto context = ready;
        uint32_t revision = 0;
        switch (fault) {
        case 0:
            grant.owner_principal_id = 0;
            break;
        case 1:
            grant.owner_binding_revision = 0;
            break;
        case 2:
            grant.capability_id = {};
            break;
        case 3:
            grant.capability_version = 0;
            break;
        case 4:
            grant.scope = nullptr;
            break;
        case 5:
            context.boot_epoch = 0;
            break;
        case 6:
            context.boot_epoch = UINT64_C(0x100000000);
            break;
        case 7:
            grant.parent_grant_id = id(1);
            break;
        case 8:
            input.field.allowed.values.integer = {20, 10};
            break;
        default:
            input.scope.struct_size += 1;
            break;
        }
        GrantTable::Admission plan{91, 92, 93, 94, 95, 96};
        const auto check = table->check_create(grant, revision, context, plan);
        EXPECT_EQ(check.status, fault == 9U ? AYNI_UNSUPPORTED_VERSION : AYNI_INVALID_ARGUMENT)
            << fault;
        EXPECT_EQ(plan.index, 91U);
        EXPECT_EQ(plan.boot_epoch, 96U);
        EXPECT_EQ(table->size(), 0U);
        input.field.allowed.values.integer = {-10, 10};
    }
}

TEST(GrantTable, complete_source_domain_must_fit_even_when_a_narrowed_token_scope_would_fit) {
    Input input;
    const ayni_string_v1 members[] = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32},
                                      {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 32}};
    ayni_authority_value_set_v1 values{};
    values.struct_size = sizeof(values);
    values.kind = AYNI_AUTHORITY_ENUM;
    values.values.tokens = {members, 2, 0};
    const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0, values};
    uint32_t size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, input.domain.data(),
                                           static_cast<uint32_t>(input.domain.size()), size),
              AYNI_OK);
    ASSERT_EQ(size, 78U);
    input.domain_size = size;
    values.values.tokens.member_count = 1;
    input.field.allowed = values;
    Grants table(1, 4, 64);
    GrantTable::Admission plan{};
    EXPECT_EQ(table->check_create(input.request(), 0, ready, plan).reason,
              AYNI_REASON_GRANT_CAPACITY);
    Grants too_small(1, 4, 4);
    Input numeric;
    EXPECT_EQ(too_small->check_create(numeric.request(), 0, ready, plan).reason,
              AYNI_REASON_GRANT_CAPACITY);
}

TEST(GrantTable,
     checked_placement_rejects_invalid_ids_stamps_and_stale_placements_without_inserting) {
    Input input;
    Grants table;
    auto grant = input.request();
    GrantTable::Admission valid{};
    ASSERT_EQ(table->check_create(grant, 0, ready, valid).status, AYNI_OK);
    for (uint32_t fault = 0; fault < 9; ++fault) {
        auto plan = valid;
        uint64_t identity = id(1);
        ayni::foundation::TransitionStamp stamp{1, 1};
        switch (fault) {
        case 0:
            identity = 0;
            break;
        case 1:
            identity = 1;
            break;
        case 2:
            identity = UINT64_C(1) << 32U;
            break;
        case 3:
            identity = id(1, 2);
            break;
        case 4:
            stamp.transition_id = 0;
            break;
        case 5:
            stamp.commit_seq = 0;
            break;
        case 6:
            plan.index = 1;
            break;
        case 7:
            plan.replaced_grant_id = id(99);
            break;
        default:
            input.field.allowed.values.integer = {10, -10};
            break;
        }
        uint32_t index = 91;
        EXPECT_FALSE(table->insert(grant, plan, identity, stamp, index)) << fault;
        EXPECT_EQ(index, 91U);
        EXPECT_EQ(table->size(), 0U);
    }
    input.field.allowed.values.integer = {-10, 10};
    ASSERT_EQ(table.add(input.request(), 1, 1).status, AYNI_OK);
    GrantTable::Admission second{};
    ASSERT_EQ(table->check_create(input.request(), 0, ready, second).status, AYNI_OK);
    uint32_t index = 91;
    EXPECT_FALSE(table->insert(input.request(), second, id(1), {2, 2}, index));
    Grants full(1);
    ASSERT_EQ(full.add(input.request(0, 0, 10), 1, 1).status, AYNI_OK);
    auto invalid_append = valid;
    invalid_append.index = 1;
    EXPECT_FALSE(full->insert(input.request(), invalid_append, id(2), {2, 2}, index));
    full->expire(0, {2, 2});
    ASSERT_EQ(full->check_create(input.request(), 0, ready, second).status, AYNI_OK);
    ++second.replaced_grant_id;
    EXPECT_FALSE(full->insert(input.request(), second, id(2), {3, 3}, index));
    EXPECT_EQ(full->at(0).grant_id, id(1));
}

TEST(GrantTable, snapshot_lifecycle_and_both_original_stamps_are_checked_independently) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(0, 0, 10), 1, 10).status, AYNI_OK);
    const auto fact = record_of(*source.operator->(), 0);
    for (uint32_t fault = 0; fault < 11; ++fault) {
        GrantSnapshotRecord snapshot{fact, {10, 10}, {10, 10}, 1, AYNI_GRANT_ACTIVE};
        switch (fault) {
        case 0:
            snapshot.created_stamp.transition_id = 0;
            break;
        case 1:
            snapshot.created_stamp.commit_seq = 0;
            break;
        case 2:
            snapshot.stamp.transition_id = 0;
            break;
        case 3:
            snapshot.stamp.commit_seq = 0;
            break;
        case 4:
            snapshot.stamp.transition_id = 11;
            break;
        case 5:
            snapshot.stamp.commit_seq = 11;
            break;
        case 6:
            snapshot.lifecycle = 3;
            break;
        case 7:
            snapshot.lifecycle = AYNI_GRANT_EXPIRED;
            break;
        case 8:
            snapshot.lifecycle = AYNI_GRANT_EXPIRED;
            snapshot.revision = 2;
            break;
        case 9:
            snapshot.lifecycle = AYNI_GRANT_EXPIRED;
            snapshot.revision = 2;
            snapshot.stamp = {11, 11};
            snapshot.create.grant.lifetime = AYNI_GRANT_PERMANENT;
            snapshot.create.grant.duration_us = 0;
            break;
        default:
            snapshot.revision = 2;
            break;
        }
        Grants restored;
        EXPECT_EQ(restored->restore_snapshot(snapshot), GrantTable::Restore::corrupt) << fault;
        EXPECT_EQ(restored->size(), 0U);
    }
    const GrantSnapshotRecord expired{fact, {10, 10}, {11, 11}, 2, AYNI_GRANT_EXPIRED};
    Grants restored;
    ASSERT_EQ(restored->restore_snapshot(expired), GrantTable::Restore::restored);
    EXPECT_EQ(restored->at(0).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(restored->at(0).deadline_us, 0U);
}

TEST(GrantTable, restore_rejects_expansion_or_issuer_schema_depth_and_lifetime_disagreements) {
    Input parent(-5, 5), child(-4, 4), wide(-6, 6);
    Grants source;
    ASSERT_EQ(source.add(parent.request(0, AYNI_GRANT_ALLOW_DELEGATION, 100), 1, 10).status,
              AYNI_OK);
    ASSERT_EQ(source.add(child.request(id(1), 0, 50), 2, 11, ready, 1).status, AYNI_OK);
    const auto parent_fact = record_of(*source.operator->(), 0);
    const auto child_fact = record_of(*source.operator->(), 1);
    std::array<uint8_t, 256> expanded{};
    uint32_t expanded_size = 0;
    ASSERT_EQ(normalize_scope({wide.domain.data(), wide.domain_size}, wide.scope, expanded.data(),
                              static_cast<uint32_t>(expanded.size()), expanded_size),
              AYNI_OK);
    for (uint32_t fault = 0; fault < 13; ++fault) {
        auto parent_record = parent_fact;
        auto record = child_fact;
        ayni::foundation::TransitionStamp stamp{11, 11};
        switch (fault) {
        case 0:
            parent_record.grant.flags = 0;
            break;
        case 1:
            record.grant.owner_principal_id += 1;
            break;
        case 2:
            record.grant.owner_binding_revision += 1;
            break;
        case 3:
            ASSERT_TRUE(record.grant.capability_id.assign({"other", 5}));
            break;
        case 4:
            record.grant.capability_version += 1;
            break;
        case 5:
            record.depth = 2;
            break;
        case 6:
            record.scope = {expanded.data(), expanded_size};
            break;
        case 7:
            stamp.transition_id = 10;
            break;
        case 8:
            stamp.commit_seq = 10;
            break;
        case 9:
            record.grant.lifetime = AYNI_GRANT_PERMANENT;
            record.grant.duration_us = 0;
            break;
        case 10:
            record.grant.duration_us = 101;
            break;
        case 11:
            record.grant_id = id(2, 2);
            break;
        default:
            record.grant.source_domain.size -= 1;
            break;
        }
        Grants restored;
        ASSERT_EQ(restored->restore_create(parent_record, {10, 10}), GrantTable::Restore::restored);
        EXPECT_EQ(restored->restore_create(record, stamp), GrantTable::Restore::corrupt) << fault;
        EXPECT_EQ(restored->size(), 1U);
    }
    Grants expired_parent;
    ASSERT_EQ(expired_parent->restore_create(parent_fact, {10, 10}), GrantTable::Restore::restored);
    ASSERT_EQ(expired_parent->restore_state({id(1), 2, AYNI_GRANT_EXPIRED}, {20, 20}),
              GrantTable::Restore::restored);
    EXPECT_EQ(expired_parent->restore_create(child_fact, {21, 21}), GrantTable::Restore::corrupt);
    GrantSnapshotRecord late_child{child_fact, {21, 21}, {21, 21}, 1, AYNI_GRANT_ACTIVE};
    EXPECT_EQ(expired_parent->restore_snapshot(late_child), GrantTable::Restore::corrupt);
    late_child.created_stamp = late_child.stamp = {11, 11};
    EXPECT_EQ(expired_parent->restore_snapshot(late_child), GrantTable::Restore::restored);
}

TEST(GrantTable, expired_child_creation_must_follow_parent_creation_even_when_terminal_is_later) {
    Input parent(-5, 5), child(-4, 4);
    Grants source;
    ASSERT_EQ(source.add(parent.request(0, AYNI_GRANT_ALLOW_DELEGATION), 1, 10).status, AYNI_OK);
    ASSERT_EQ(source.add(child.request(id(1), 0, 10), 2, 11, ready, 1).status, AYNI_OK);
    const auto parent_fact = record_of(*source.operator->(), 0);
    const auto child_fact = record_of(*source.operator->(), 1);
    Grants restored;
    ASSERT_EQ(restored->restore_create(parent_fact, {10, 10}), GrantTable::Restore::restored);
    const GrantSnapshotRecord impossible{child_fact, {9, 9}, {20, 20}, 2, AYNI_GRANT_EXPIRED};
    EXPECT_EQ(restored->restore_snapshot(impossible), GrantTable::Restore::corrupt);
    ASSERT_EQ(restored->size(), 1U);
    EXPECT_EQ(restored->at(0).grant_id, id(1));
    EXPECT_EQ(restored->at(0).revision, 1U);
    EXPECT_EQ(restored->at(0).stamp.commit_seq, 10U);
    const GrantSnapshotRecord legal{child_fact, {11, 11}, {20, 20}, 2, AYNI_GRANT_EXPIRED};
    ASSERT_EQ(restored->restore_snapshot(legal), GrantTable::Restore::restored);
    EXPECT_EQ(restored->size(), 2U);
    EXPECT_EQ(restored->at(1).created_stamp.commit_seq, 11U);
    EXPECT_EQ(restored->at(1).stamp.commit_seq, 20U);
    EXPECT_TRUE(restored->consistent());
}

TEST(GrantTable, restore_state_and_replacement_validate_exact_old_facts_without_partial_mutation) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(0, AYNI_GRANT_ALLOW_DELEGATION, 10), 1, 1).status, AYNI_OK);
    const auto first = record_of(*source.operator->(), 0);
    Grants restored;
    ASSERT_EQ(restored->restore_create(first, {1, 1}), GrantTable::Restore::restored);
    EXPECT_EQ(restored->restore_create(first, {2, 2}), GrantTable::Restore::corrupt);
    for (uint32_t fault = 0; fault < 7; ++fault) {
        GrantStateRecord state{id(1), 2, AYNI_GRANT_EXPIRED};
        ayni::foundation::TransitionStamp stamp{2, 2};
        switch (fault) {
        case 0:
            state.grant_id = id(99);
            break;
        case 1:
            state.revision = 3;
            break;
        case 2:
            state.lifecycle = AYNI_GRANT_ACTIVE;
            break;
        case 3:
            stamp.transition_id = 0;
            break;
        case 4:
            stamp.commit_seq = 0;
            break;
        case 5:
            stamp.transition_id = 1;
            break;
        default:
            stamp.commit_seq = 1;
            break;
        }
        EXPECT_EQ(restored->restore_state(state, stamp), GrantTable::Restore::corrupt) << fault;
        EXPECT_EQ(restored->at(0).revision, 1U);
    }
    auto replacement = first;
    replacement.grant_id = id(2);
    replacement.replaced_grant_id = id(99);
    EXPECT_EQ(restored->restore_create(replacement, {3, 3}), GrantTable::Restore::corrupt);
    replacement.replaced_grant_id = id(1);
    EXPECT_EQ(restored->restore_create(replacement, {3, 3}), GrantTable::Restore::corrupt);
    ASSERT_EQ(restored->restore_state({id(1), 2, AYNI_GRANT_EXPIRED}, {2, 2}),
              GrantTable::Restore::restored);
    EXPECT_EQ(restored->restore_state({id(1), 2, AYNI_GRANT_EXPIRED}, {3, 3}),
              GrantTable::Restore::corrupt);
    EXPECT_EQ(restored->restore_create(replacement, {2, 2}), GrantTable::Restore::corrupt);
    ASSERT_EQ(restored->restore_create(replacement, {3, 3}), GrantTable::Restore::restored);
    restored->clear();
    uint32_t index = 91;
    EXPECT_FALSE(restored->find(0, index));
    EXPECT_FALSE(restored->find(id(2), index));
    EXPECT_EQ(index, 91U);
    EXPECT_EQ(restored->check_chain(0, ready).reason, AYNI_REASON_GRANT_UNKNOWN);
}

TEST(GrantTable, all_old_boot_expiry_is_atomic_and_equal_deadlines_use_id_order) {
    Input input;
    Grants source;
    ASSERT_EQ(source.add(input.request(0, 0, 10), 2, 10).status, AYNI_OK);
    ASSERT_EQ(source.add(input.request(0, 0, 10), 1, 20).status, AYNI_OK);
    uint32_t selected = 91;
    uint64_t deadline = 92;
    ASSERT_TRUE(source->next_expiry(ready, selected, deadline));
    EXPECT_EQ(source->at(selected).grant_id, id(1));
    Grants restored;
    ASSERT_EQ(restored->restore_create(record_of(*source.operator->(), 0), {10, 10}),
              GrantTable::Restore::restored);
    ASSERT_EQ(restored->restore_create(record_of(*source.operator->(), 1), {20, 20}),
              GrantTable::Restore::restored);
    restored->expire_old_boot(2, {0, 21});
    restored->expire_old_boot(2, {21, 0});
    restored->expire_old_boot(2, {15, 15});
    EXPECT_EQ(restored->at(0).revision, 1U);
    EXPECT_EQ(restored->at(1).revision, 1U);
    restored->expire_old_boot(2, {21, 21});
    EXPECT_EQ(restored->at(0).stamp.commit_seq, 21U);
    EXPECT_EQ(restored->at(1).stamp.commit_seq, 21U);
    selected = 91;
    deadline = 92;
    EXPECT_FALSE(restored->next_expiry(ready, selected, deadline));
    EXPECT_EQ(selected, 91U);
    EXPECT_EQ(deadline, 92U);
}

TEST(GrantTable, chain_checks_fail_closed_when_retained_facts_or_ancestor_links_are_corrupt) {
    Input parent(-5, 5), child(-4, 4);
    for (uint32_t fault = 0; fault < 17; ++fault) {
        Grants table;
        ASSERT_EQ(table.add(parent.request(0, AYNI_GRANT_ALLOW_DELEGATION, 100), 1, 10).status,
                  AYNI_OK);
        ASSERT_EQ(table.add(child.request(id(1), 0, 50), 2, 11, ready, 1).status, AYNI_OK);
        // White-box corruption injection checks the authorization guard on
        // retained facts. No malformed state is supplied as a legal command.
        auto &ancestor = const_cast<Grant &>(table->at(0));
        auto &leaf = const_cast<Grant &>(table->at(1));
        auto context = ready;
        switch (fault) {
        case 0:
            ancestor.allow_delegation = false;
            break;
        case 1:
            leaf.parent_grant_id = id(99);
            break;
        case 2:
            leaf.parent_grant_id = 0;
            break;
        case 3:
            leaf.depth = 0;
            break;
        case 4:
            ancestor.depth = 4;
            leaf.depth = 5;
            break;
        case 5:
            leaf.owner_principal_id += 1U;
            break;
        case 6:
            leaf.owner_binding_revision += 1U;
            break;
        case 7:
            ASSERT_TRUE(leaf.capability_id.assign({"other", 5}));
            break;
        case 8:
            leaf.capability_version += 1U;
            break;
        case 9:
            leaf.source_domain_size -= 1U;
            break;
        case 10:
            leaf.lifetime = AYNI_GRANT_PERMANENT;
            leaf.duration_us = 0;
            break;
        case 11:
            leaf.grant_id = id(2, 2);
            context.boot_epoch = 2;
            break;
        case 12:
            leaf.duration_us = 101;
            break;
        case 13:
            leaf.deadline_us = 201;
            break;
        case 14:
            const_cast<uint8_t *>(table->scope(1).data)[0] = 0;
            break;
        case 15:
            // Still within the registered [-10,10], beyond its parent's [-5,5].
            ayni::foundation::store_le64(const_cast<uint8_t *>(table->scope(1).data) + 17U,
                                         static_cast<uint64_t>(int64_t{-6}));
            break;
        default:
            ancestor.depth = 1;
            break;
        }
        EXPECT_EQ(table->check_chain(1, context).reason, AYNI_REASON_GRANT_PARENT) << fault;
        if (fault != 10U && fault != 11U && fault != 12U && fault != 13U) {
            EXPECT_FALSE(table->consistent()) << fault;
        }
    }
}

TEST(GrantTable, a_new_catalog_version_is_an_ephemeral_hold_and_existing_hold_has_priority) {
    Input input;
    Grants table;
    ASSERT_EQ(table.add(input.request(), 1, 1).status, AYNI_OK);
    std::vector<uint64_t> memory(
        ayni::catalog::CapabilityRegistry::storage_size(1, 256) / sizeof(uint64_t) + 1U);
    ayni::catalog::CapabilityRegistry catalog(memory.data(), 1, 256);
    auto values = input.field.allowed;
    const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0, values};
    const ayni_capability_desc_v1 desc{sizeof(desc), 2, {"test.cap", 8}, 1, 0, &field};
    ASSERT_EQ(catalog.add(desc), AYNI_OK);
    table->resolve_catalog(catalog);
    EXPECT_EQ(table->check_chain(0, ready).status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(table->check_chain(0, ready).reason, AYNI_REASON_CAPABILITY_VERSION);
    table->hold_restored(0, AYNI_REASON_GRANT_SUBJECT);
    EXPECT_EQ(table->at(0).hold_reason, AYNI_REASON_CAPABILITY_VERSION);
    EXPECT_EQ(table->at(0).stamp.commit_seq, 1U);
}

} // namespace
