// ADR-0031: pending controls survive restored-state discard, share exact
// command identity with every kind, and retain applied results until durable.
#include "kernel/command/command_table.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace ayni::kernel;
using Lookup = CommandTable::Lookup;
constexpr uint64_t owner = 7;

AcceptedRevoke accepted(uint64_t command = 5, uint64_t target = 99) {
    return {owner, command, 3, 1, target};
}
CommandMapping ordinary(uint64_t command, uint64_t seq) {
    return {owner,
            command,
            command + 1000,
            seq,
            AYNI_INTENT_GOAL_CREATE,
            {0, command + 500, 1, AYNI_OBJECT_GOAL}};
}
CommandMapping result(const PendingRevoke &control, uint64_t seq,
                      ayni_intent_effect_t effect = AYNI_INTENT_EFFECT_CHANGED) {
    return {control.accepted.principal_id,
            control.accepted.command_id,
            0,
            seq,
            AYNI_INTENT_GRANT_REVOKE,
            {0, control.accepted.target_grant_id,
             effect == AYNI_INTENT_EFFECT_TARGET_GONE ? 0U : 2U, AYNI_OBJECT_GRANT},
            0,
            effect};
}
ayni_grant_create_v1 create_request() {
    ayni_grant_create_v1 grant{};
    grant.struct_size = sizeof(grant);
    grant.capability_version = 1;
    grant.capability_id = {"a", 1};
    grant.subject_instance_id = 1;
    grant.lifetime = AYNI_GRANT_PERMANENT;
    grant.scope.struct_size = sizeof(grant.scope);
    return grant;
}
class Commands {
  public:
    Commands(uint32_t capacity = 2, uint32_t controls = 2, uint32_t bytes = 340, uint32_t rows = 1)
        : storage_(CommandTable::storage_size(rows, capacity, bytes, controls) / sizeof(uint64_t) +
                   1),
          table(storage_.data(), rows, capacity, bytes, controls) {}
    const PendingRevoke *accept(uint64_t command = 5, uint64_t target = 99) {
        const PendingRevoke *control = nullptr;
        EXPECT_EQ(table.accept_revoke(accepted(command, target), control), AYNI_OK);
        EXPECT_NE(control, nullptr);
        return control;
    }

  private:
    std::vector<uint64_t> storage_;

  public:
    CommandTable table;
};

// ADR-0031 / C-WIRE-017: a revoke lookup is not authority acceptance and
// cannot supply a create candidate; accepted control owns its own exact bytes.
TEST(PendingRevoke, revoke_lookup_invalidates_create_scratch_without_staging_a_publishable_result) {
    Commands commands;
    const auto create = create_request();
    ASSERT_EQ(commands.table.find_exact(owner, 5, CanonicalCommandView{0, &create}).lookup,
              Lookup::unmapped);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, 99};
    const auto found = commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &revoke});
    ASSERT_EQ(found.status, AYNI_OK);
    ASSERT_EQ(found.lookup, Lookup::unmapped);
    EXPECT_EQ(found.mapping, nullptr);
    EXPECT_EQ(found.pending, nullptr);
    CommandMapping mapping{owner, 5, 0, 9, AYNI_INTENT_GRANT_CREATE, {0, 99, 1, AYNI_OBJECT_GRANT}};
    EXPECT_FALSE(commands.table.insert_exact(mapping));
    mapping.kind = AYNI_INTENT_GRANT_REVOKE;
    mapping.object.revision = 2;
    EXPECT_FALSE(commands.table.insert_exact(mapping));
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(commands.table.pending_size(), 0U);
    EXPECT_EQ(commands.table.floor(owner), 0U);

    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    const auto canonical = control->canonical;
    ASSERT_EQ(commands.table.find_exact(owner, 6, CanonicalCommandView{0, &create}).lookup,
              Lookup::unmapped);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 3));
    const auto *published = commands.table.mapped(owner, 5);
    ASSERT_NE(published, nullptr);
    const auto bytes = commands.table.canonical(*published);
    ASSERT_EQ(bytes.size, canonical.size());
    EXPECT_EQ(std::memcmp(bytes.data, canonical.data(), bytes.size), 0);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(PendingRevoke, acceptance_copies_identity_without_publishing_a_window_or_queued_stamp) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->accepted.binding_revision, 3U);
    EXPECT_EQ(control->accepted.target_grant_id, 99U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_EQ(control->queued_boot_epoch, 0U);
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(commands.table.floor(owner), 0U);
    EXPECT_EQ(commands.table.next_pending_revoke(), control);
    const PendingRevoke *retry = nullptr;
    EXPECT_EQ(commands.table.accept_revoke(accepted(), retry), AYNI_OK);
    EXPECT_EQ(retry, control);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    const auto canonical = control->canonical;
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, 99};
    const auto found = commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &revoke});
    EXPECT_EQ(found.lookup, Lookup::retried);
    EXPECT_EQ(found.pending, control);
    EXPECT_EQ(found.mapping, nullptr);
    EXPECT_EQ(control->canonical, canonical);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, control_capacity_and_same_target_refusals_are_atomic) {
    Commands commands(1, 1);
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    const auto canonical = control->canonical;
    const PendingRevoke *output = control;
    EXPECT_EQ(commands.table.can_accept_revoke(accepted(6, 99)), AYNI_BUSY);
    EXPECT_EQ(commands.table.accept_revoke(accepted(6, 100), output), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(output, control);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(commands.table.floor(owner), 0U);
    EXPECT_EQ(control->canonical, canonical);
    EXPECT_FALSE(commands.table.insert(result(*control, 9))); // No digest-only auth insertion.
    auto conflict = accepted();
    ++conflict.expected_revision;
    EXPECT_EQ(commands.table.accept_revoke(conflict, output), AYNI_IDEMPOTENCY_CONFLICT);
    conflict = accepted();
    ++conflict.binding_revision;
    EXPECT_EQ(commands.table.can_accept_revoke(conflict), AYNI_IDEMPOTENCY_CONFLICT);
    conflict = accepted();
    conflict.principal_id = 0;
    EXPECT_EQ(commands.table.accept_revoke(conflict, output), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output, control);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, pending_identity_precedes_all_kinds_window_and_a_later_floor) {
    Commands commands(1, 1);
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(commands.table.insert(ordinary(50, 1)));
    ASSERT_TRUE(commands.table.insert(ordinary(60, 2)));
    ASSERT_EQ(commands.table.floor(owner), 50U);
    const CommandMapping *mapping = nullptr;
    EXPECT_EQ(commands.table.find(owner, 5, 1005, mapping), Lookup::conflict);
    const auto create = create_request();
    EXPECT_EQ(commands.table.find_exact(owner, 5, CanonicalCommandView{0, &create}).lookup,
              Lookup::conflict);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, 99};
    EXPECT_EQ(commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &revoke}).lookup,
              Lookup::retried);
    EXPECT_EQ(commands.table.find_exact(owner, 5, CanonicalRevokeView{2, &revoke}).lookup,
              Lookup::conflict);
    const PendingRevoke *receipt = nullptr;
    EXPECT_EQ(commands.table.lookup_receipt(owner, 5, receipt, mapping), Lookup::retried);
    EXPECT_EQ(receipt, control);
    EXPECT_EQ(mapping, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 3), 2));
    EXPECT_EQ(commands.table.floor(owner), 60U);
    EXPECT_EQ(commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &revoke}).lookup,
              Lookup::retried);
    EXPECT_EQ(commands.table.size(), 1U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, applied_result_is_pinned_until_exact_boot_durability_handoff) {
    Commands commands(1, 2);
    const auto *first = commands.accept(5, 99);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*first, result(*first, 9), 3));
    EXPECT_EQ(first->queued_boot_epoch, 3U);
    EXPECT_EQ(first->queued_commit_seq, 9U);
    EXPECT_FALSE(commands.table.can_insert(owner));
    EXPECT_FALSE(commands.table.insert(ordinary(50, 10)));
    const auto create = create_request();
    ASSERT_EQ(commands.table.find_exact(owner, 50, CanonicalCommandView{0, &create}).lookup,
              Lookup::unmapped);
    const CommandMapping grant{
        owner, 50, 0, 10, AYNI_INTENT_GRANT_CREATE, {0, 1000, 1, AYNI_OBJECT_GRANT}};
    EXPECT_FALSE(commands.table.insert_exact(grant));
    EXPECT_EQ(commands.table.floor(owner), 0U);
    EXPECT_EQ(commands.table.size(), 1U);
    const auto *second = commands.accept(6, 100);
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(commands.table.can_publish_accepted(*second));
    EXPECT_FALSE(commands.table.publish_accepted_revoke(*second, result(*second, 10), 3));
    EXPECT_EQ(commands.table.next_durable_pending(4, 100), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(3, 8), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(3, 9), first);
    ASSERT_TRUE(commands.table.retire_pending(owner, 5));
    EXPECT_TRUE(commands.table.can_publish_accepted(*second));
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*second, result(*second, 10), 3));
    EXPECT_EQ(commands.table.floor(owner), 5U);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(commands.table.mapped(owner, 6)->retention, CommandRetention::accepted_revoke);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, eviction_moves_pinned_metadata_and_canonical_together) {
    Commands commands(2, 1);
    ASSERT_TRUE(commands.table.insert(ordinary(1, 1)));
    const auto *control = commands.accept(5, 99);
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 2), 3));
    ASSERT_TRUE(commands.table.insert(ordinary(6, 3))); // Move pinned last slot into evicted first.
    EXPECT_EQ(commands.table.floor(owner), 1U);
    const CommandMapping *mapping = nullptr;
    EXPECT_EQ(commands.table.recovered_revoke(*control, mapping), Lookup::retried);
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->retention, CommandRetention::accepted_revoke);
    EXPECT_EQ(commands.table.canonical(*mapping).size, 16U);
    ASSERT_TRUE(commands.table.insert(ordinary(7, 4))); // Evict 6, never pinned 5.
    EXPECT_EQ(commands.table.floor(owner), 6U);
    EXPECT_EQ(commands.table.recovered_revoke(*control, mapping), Lookup::retried);
    EXPECT_EQ(mapping->object.id, 99U);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(PendingRevoke, discard_retains_control_but_never_compares_an_old_queued_seq_in_a_new_boot) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 3));
    commands.table.clear_restored();
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(commands.table.pending(owner, 5), control);
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(control->queued_boot_epoch, 0U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_EQ(commands.table.next_pending_revoke(), control);
    ASSERT_TRUE(commands.table.insert(ordinary(50, 9))); // The old seq is now unrelated.
    EXPECT_EQ(commands.table.next_durable_pending(4, 100), nullptr);
    const CommandMapping *mapping = nullptr;
    EXPECT_EQ(commands.table.recovered_revoke(*control, mapping), Lookup::unmapped);
    EXPECT_FALSE(commands.table.retire_pending(owner, 5));
    ASSERT_TRUE(commands.table.publish_accepted_revoke(
        *control, result(*control, 10, AYNI_INTENT_EFFECT_TARGET_GONE), 4));
    EXPECT_EQ(commands.table.next_durable_pending(4, 10), control);
    ASSERT_TRUE(commands.table.retire_pending(owner, 5));
    EXPECT_EQ(commands.table.mapped(owner, 5)->effect, AYNI_INTENT_EFFECT_TARGET_GONE);
    commands.table.clear();
    EXPECT_EQ(commands.table.pending_size(), 0U);
    EXPECT_EQ(commands.table.size(), 0U);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(PendingRevoke, recovered_complete_canonical_is_the_only_cross_reload_confirmation) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 3));
    const auto original = *commands.table.mapped(owner, 5);
    const auto canonical = control->canonical;
    commands.table.clear_restored();
    ASSERT_EQ(commands.table.restore_checked(original, 0, {canonical.data(), 16}),
              CommandTable::Restore::restored);
    const CommandMapping *restored = nullptr;
    EXPECT_EQ(commands.table.recovered_revoke(*control, restored), Lookup::retried);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->commit_seq, 9U);
    EXPECT_EQ(restored->retention, CommandRetention::normal); // Pin is not a persistent fact.
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_EQ(commands.table.next_durable_pending(4, 100), nullptr);
    ASSERT_TRUE(commands.table.retire_pending(owner, 5));
    EXPECT_EQ(commands.table.pending_size(), 0U);
    const PendingRevoke *pending = nullptr;
    EXPECT_EQ(commands.table.lookup_receipt(owner, 5, pending, restored), Lookup::retried);
    EXPECT_EQ(pending, nullptr);
    EXPECT_EQ(restored->effect, AYNI_INTENT_EFFECT_CHANGED);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, failed_publication_or_foreign_control_changes_no_window_or_floor) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    auto bad = result(*control, 9);
    bad.object.id = 100;
    EXPECT_FALSE(commands.table.publish_accepted_revoke(*control, bad, 3));
    EXPECT_FALSE(commands.table.publish_accepted_revoke(*control, result(*control, 0), 3));
    EXPECT_FALSE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 0));
    const auto copy = *control;
    EXPECT_FALSE(commands.table.can_publish_accepted(copy));
    EXPECT_FALSE(commands.table.publish_accepted_revoke(copy, result(copy, 9), 3));
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(commands.table.floor(owner), 0U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    // An odd byte stride must not misalign the pending metadata placed before it.
    Commands odd_stride(1, 1, 17);
    const auto *odd = odd_stride.accept();
    ASSERT_NE(odd, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(odd) % alignof(PendingRevoke), 0U);
    EXPECT_TRUE(odd_stride.table.publish_accepted_revoke(*odd, result(*odd, 1), 2));
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, conflicting_restored_identity_cannot_release_an_accepted_control) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    commands.table.clear_restored();
    ASSERT_EQ(commands.table.restore_checked(ordinary(5, 9), 0), CommandTable::Restore::restored);
    const CommandMapping *mapping = nullptr;
    EXPECT_EQ(commands.table.recovered_revoke(*control, mapping), Lookup::conflict);
    EXPECT_EQ(mapping, nullptr);
    EXPECT_FALSE(commands.table.retire_pending(owner, 5));
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, full_revoke_identity_still_wins_when_the_window_digest_collides) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 3));
    ASSERT_TRUE(commands.table.retire_pending(owner, 5));
    const ayni_grant_revoke_v1 other{sizeof(other), 0, 100};
    std::array<uint8_t, 16> other_bytes{};
    uint32_t size = 0;
    ASSERT_EQ(encode_revoke_command({1, &other}, other_bytes.data(), 16, size), AYNI_OK);
    // White-box forced index collision; this does not claim a real FNV collision.
    auto *mapping = const_cast<CommandMapping *>(commands.table.mapped(owner, 5));
    ASSERT_NE(mapping, nullptr);
    mapping->digest = authority_command_digest({other_bytes.data(), 16});
    const auto found = commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &other});
    EXPECT_EQ(found.lookup, Lookup::conflict);
    EXPECT_EQ(found.mapping, nullptr);
    EXPECT_EQ(mapping->object.id, 99U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, malformed_requests_and_zero_identity_leave_the_accepted_control_untouched) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    const auto original = control->canonical;
    const ayni_grant_revoke_v1 body{sizeof(body), 0, 99};
    const std::array<CanonicalRevokeView, 2> malformed{{{0, &body}, {1, nullptr}}};
    for (const auto &request : malformed) {
        EXPECT_EQ(commands.table.find_exact(owner, 6, request).status, AYNI_INVALID_ARGUMENT);
    }
    EXPECT_EQ(commands.table.find_exact(0, 6, CanonicalRevokeView{1, &body}).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(commands.table.find_exact(owner, 0, CanonicalRevokeView{1, &body}).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(commands.table.pending(0, 5), nullptr);
    EXPECT_EQ(commands.table.pending(owner, 0), nullptr);
    EXPECT_FALSE(commands.table.can_insert(0));
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(commands.table.size(), 0U);
    EXPECT_EQ(control->canonical, original);
    EXPECT_EQ(control->queued_commit_seq, 0U);
    EXPECT_EQ(commands.table.floor(owner), 0U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, every_missing_accepted_identity_component_refuses_without_consuming_capacity) {
    Commands commands(2, 1);
    const PendingRevoke sentinel{};
    const PendingRevoke *out = &sentinel;
    std::array<AcceptedRevoke, 5> malformed{};
    malformed.fill(accepted());
    malformed[0].principal_id = 0;
    malformed[1].command_id = 0;
    malformed[2].binding_revision = 0;
    malformed[3].expected_revision = 0;
    malformed[4].target_grant_id = 0;
    for (const auto &request : malformed) {
        EXPECT_EQ(commands.table.can_accept_revoke(request), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(commands.table.accept_revoke(request, out), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(out, &sentinel);
        EXPECT_EQ(commands.table.pending_size(), 0U);
        EXPECT_EQ(commands.table.size(), 0U);
        EXPECT_EQ(commands.table.floor(owner), 0U);
    }
    ASSERT_EQ(commands.table.accept_revoke(accepted(), out), AYNI_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->accepted.command_id, 5U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, mapped_and_expired_commands_cannot_be_accepted_as_new_controls) {
    Commands commands(1, 1);
    ASSERT_TRUE(commands.table.insert(ordinary(5, 1)));
    const PendingRevoke sentinel{};
    const PendingRevoke *out = &sentinel;
    EXPECT_EQ(commands.table.accept_revoke(accepted(), out), AYNI_INVALID_STATE);
    EXPECT_EQ(out, &sentinel);
    ASSERT_TRUE(commands.table.insert(ordinary(6, 2)));
    ASSERT_EQ(commands.table.floor(owner), 5U);
    EXPECT_EQ(commands.table.accept_revoke(accepted(), out), AYNI_EXPIRED);
    EXPECT_EQ(out, &sentinel);
    const ayni_grant_revoke_v1 body{sizeof(body), 0, 99};
    EXPECT_EQ(commands.table.find_exact(owner, 5, CanonicalRevokeView{1, &body}).lookup,
              Lookup::expired);
    EXPECT_EQ(commands.table.find_exact(owner, 6, CanonicalRevokeView{1, &body}).lookup,
              Lookup::conflict);
    EXPECT_EQ(commands.table.pending_size(), 0U);
    EXPECT_EQ(commands.table.size(), 1U);
    EXPECT_EQ(commands.table.at(0).command_id, 6U);
    EXPECT_EQ(commands.table.floor(owner), 5U);
    const PendingRevoke *receipt = nullptr;
    const CommandMapping *mapping = commands.table.mapped(owner, 6);
    EXPECT_EQ(commands.table.lookup_receipt(owner, 5, receipt, mapping), Lookup::expired);
    EXPECT_EQ(receipt, nullptr);
    EXPECT_EQ(mapping, nullptr);
    EXPECT_EQ(commands.table.lookup_receipt(owner, 7, receipt, mapping), Lookup::unmapped);
    EXPECT_EQ(receipt, nullptr);
    EXPECT_EQ(mapping, nullptr);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, ordinary_only_windows_explicitly_refuse_exact_revoke_storage) {
    Commands legacy(1, 0, 0);
    ASSERT_TRUE(legacy.table.insert(ordinary(5, 1)));
    const ayni_grant_revoke_v1 body{sizeof(body), 0, 99};
    EXPECT_EQ(legacy.table.find_exact(owner, 6, CanonicalRevokeView{1, &body}).status,
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(legacy.table.can_accept_revoke(accepted(6)), AYNI_CAPACITY_EXCEEDED);
    EXPECT_FALSE(legacy.table.can_insert(owner + 1U));
    EXPECT_EQ(legacy.table.size(), 1U);
    EXPECT_EQ(legacy.table.at(0).command_id, 5U);
    EXPECT_EQ(legacy.table.floor(owner), 0U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, incomplete_pending_queries_never_replace_the_callers_match) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    const PendingRevoke sentinel{};
    const PendingRevoke *out = &sentinel;
    EXPECT_EQ(commands.table.find_pending(owner, 5, AYNI_INTENT_GRANT_REVOKE,
                                          {nullptr, revoke_command_size}, out),
              Lookup::conflict);
    EXPECT_EQ(out, &sentinel);
    EXPECT_EQ(commands.table.find_pending(owner, 5, AYNI_INTENT_GRANT_REVOKE,
                                          {control->canonical.data(), revoke_command_size - 1U},
                                          out),
              Lookup::conflict);
    EXPECT_EQ(out, &sentinel);
    auto different = accepted();
    ++different.target_grant_id;
    EXPECT_EQ(commands.table.accept_revoke(different, out), AYNI_IDEMPOTENCY_CONFLICT);
    EXPECT_EQ(out, &sentinel);
    EXPECT_EQ(commands.table.pending_size(), 1U);
    EXPECT_EQ(control->queued_commit_seq, 0U);
}

// C-AUTH-024 / C-WIRE-017: accepted denial retains one exact pending identity.
TEST(PendingRevoke, publication_is_bound_to_the_accepted_principal_command_and_kind) {
    Commands commands;
    const auto *control = commands.accept();
    ASSERT_NE(control, nullptr);
    std::array<CommandMapping, 3> incorrect{};
    incorrect.fill(result(*control, 9));
    ++incorrect[0].principal_id;
    ++incorrect[1].command_id;
    incorrect[2].kind = AYNI_INTENT_GOAL_CREATE;
    for (const auto &mapping : incorrect) {
        EXPECT_FALSE(commands.table.publish_accepted_revoke(*control, mapping, 3));
        EXPECT_EQ(commands.table.size(), 0U);
        EXPECT_EQ(commands.table.floor(owner), 0U);
        EXPECT_EQ(control->queued_commit_seq, 0U);
        EXPECT_EQ(control->queued_boot_epoch, 0U);
    }
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*control, result(*control, 9), 3));
    const auto foreign_copy = *control;
    const CommandMapping *out = commands.table.mapped(owner, 5);
    const auto *original = out;
    EXPECT_EQ(commands.table.recovered_revoke(foreign_copy, out), Lookup::conflict);
    EXPECT_EQ(out, original);
    EXPECT_FALSE(commands.table.publish_accepted_revoke(*control, result(*control, 10), 3));
    EXPECT_EQ(control->queued_commit_seq, 9U);
    EXPECT_EQ(commands.table.size(), 1U);
    EXPECT_EQ(commands.table.floor(owner), 0U);
}

// C-AUTH-027 / ADR-0031: receipts and handoff require the correct durable identity.
TEST(PendingRevoke, multiple_controls_follow_target_order_then_current_boot_durable_sequence) {
    Commands commands(3, 3);
    const auto *higher = commands.accept(5, 100);
    const auto *lower = commands.accept(6, 99);
    const auto *later = commands.accept(7, 101);
    ASSERT_NE(higher, nullptr);
    ASSERT_NE(lower, nullptr);
    ASSERT_NE(later, nullptr);
    // Slot order reflects acceptance; publication follows deterministic target
    // order and gets increasing transition sequences from the coordinator.
    EXPECT_EQ(commands.table.next_pending_revoke(), lower);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*lower, result(*lower, 9), 3));
    EXPECT_EQ(commands.table.next_pending_revoke(), higher);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*higher, result(*higher, 10), 3));
    EXPECT_EQ(commands.table.next_pending_revoke(), later);
    ASSERT_TRUE(commands.table.publish_accepted_revoke(*later, result(*later, 11), 3));
    EXPECT_EQ(commands.table.next_pending_revoke(), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(0, UINT64_MAX), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(4, UINT64_MAX), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(3, 8), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(3, 10), lower);
    EXPECT_EQ(commands.table.next_durable_pending(3, 11), lower);
    ASSERT_TRUE(commands.table.retire_pending(owner, 6));
    EXPECT_FALSE(commands.table.retire_pending(owner, 6));
    EXPECT_EQ(commands.table.next_durable_pending(3, 10), higher);
    ASSERT_TRUE(commands.table.retire_pending(owner, 5));
    EXPECT_EQ(commands.table.next_durable_pending(3, 10), nullptr);
    EXPECT_EQ(commands.table.next_durable_pending(3, 11), later);
    ASSERT_TRUE(commands.table.retire_pending(owner, 7));
    EXPECT_EQ(commands.table.pending_size(), 0U);
    EXPECT_TRUE(commands.table.can_insert(owner));
}
} // namespace
