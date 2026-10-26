// Management command idempotency in the kernel (C-WIRE-003, C-WIRE-004,
// ADR-0026): what find() makes of a command, the bounded row of each
// principal, the mapping that gives way when a row is full and the floor it
// leaves (ADR-0026 decision 9), the mapping a transition journals, and the
// replay of COMMAND_MAPPING records.

#include "kernel/service/command/command_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

using ayni::kernel::CanonicalCommandView;
using ayni::kernel::CommandMapping;
using ayni::kernel::CommandTable;
using Lookup = CommandTable::Lookup;

constexpr uint64_t owner = 7;
constexpr uint64_t other = 8;
constexpr uint64_t third = 9;

static_assert(CommandTable::storage_alignment <= alignof(uint64_t), "vector storage suffices");

// The digest of what the principal accepted as command_id; any other value
// stands for other content.
uint64_t digest_of(uint64_t principal_id, uint64_t command_id) {
    return command_id * 1000U + principal_id;
}

// The mapping of command_id, accepted in the transition commit_seq.
CommandMapping mapping(uint64_t principal_id, uint64_t command_id, uint64_t commit_seq = 0) {
    return {principal_id,
            command_id,
            digest_of(principal_id, command_id),
            commit_seq,
            AYNI_INTENT_GOAL_CREATE,
            {0, command_id + 500U, 1, AYNI_OBJECT_GOAL}};
}

class Commands {
  public:
    Commands(uint32_t rows, uint32_t capacity)
        : storage_(CommandTable::storage_size({rows, capacity}) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), {rows, capacity}) {}

    CommandTable *operator->() { return &table_; }

    // With the digest the command was accepted with.
    Lookup find(uint64_t principal_id, uint64_t command_id) const {
        const CommandMapping *found = nullptr;
        return table_.find(principal_id, command_id, digest_of(principal_id, command_id), found);
    }

    // Maps a command find() returns unmapped for.
    void insert(uint64_t principal_id, uint64_t command_id, uint64_t commit_seq = 0) {
        ASSERT_EQ(find(principal_id, command_id), Lookup::unmapped) << command_id;
        ASSERT_TRUE(table_.has_row(principal_id));
        table_.insert(mapping(principal_id, command_id, commit_seq));
    }

  private:
    std::vector<uint64_t> storage_;
    CommandTable table_;
};

// C-WIRE-003: a retry of an accepted command, same content, finds the result
// the first one got.
TEST(CommandTable, accepted_command_is_retried_with_its_result) {
    Commands commands(1, 4);
    EXPECT_EQ(commands.find(owner, 5), Lookup::unmapped);
    commands.insert(owner, 5, 3);

    const CommandMapping *found = nullptr;
    ASSERT_EQ(commands->find(owner, 5, digest_of(owner, 5), found), Lookup::retried);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->principal_id, owner);
    EXPECT_EQ(found->command_id, 5U);
    EXPECT_EQ(found->commit_seq, 3U);
    EXPECT_EQ(found->kind, AYNI_INTENT_GOAL_CREATE);
    EXPECT_EQ(found->object.kind, AYNI_OBJECT_GOAL);
    EXPECT_EQ(found->object.id, 505U);
    EXPECT_EQ(found->object.revision, 1U);
    EXPECT_EQ(commands.find(owner, 6), Lookup::unmapped);
    EXPECT_EQ(commands->floor(owner), 0U);
}

// C-WIRE-004: the same command id with other content is a conflict, never
// the first result.
TEST(CommandTable, same_command_id_with_other_content_is_a_conflict) {
    Commands commands(1, 4);
    commands.insert(owner, 5);
    const CommandMapping *found = nullptr;
    EXPECT_EQ(commands->find(owner, 5, digest_of(owner, 5) + 1U, found), Lookup::conflict);
    EXPECT_EQ(found, nullptr);
}

// Command ids are the principal's own: each principal has a row, and a
// principal without one finds nothing mapped.
TEST(CommandTable, each_principal_has_a_row_of_its_own) {
    Commands commands(2, 2);
    EXPECT_TRUE(commands->has_row(owner));
    EXPECT_TRUE(commands->has_row(other));
    commands.insert(owner, 5);
    EXPECT_EQ(commands.find(other, 5), Lookup::unmapped);
    const CommandMapping *found = nullptr;
    EXPECT_EQ(commands->find(other, 5, digest_of(owner, 5), found), Lookup::unmapped);
    commands.insert(other, 5);
    EXPECT_EQ(commands.find(owner, 5), Lookup::retried);
    EXPECT_EQ(commands.find(other, 5), Lookup::retried);

    // Both rows are taken: the principals keep theirs, a third gets none.
    EXPECT_TRUE(commands->has_row(owner));
    EXPECT_TRUE(commands->has_row(other));
    EXPECT_FALSE(commands->has_row(third));
    EXPECT_EQ(commands.find(third, 5), Lookup::unmapped);
    EXPECT_EQ(commands->floor(third), 0U);
}

// ADR-0026 decision 9: a full row gives way at its smallest command id and
// raises the floor to it, so that id and every unmapped one below it are
// expired.
TEST(CommandTable, full_row_gives_way_at_its_smallest_command_id) {
    Commands commands(1, 3);
    commands.insert(owner, 5);
    commands.insert(owner, 2);
    commands.insert(owner, 9);
    commands.insert(owner, 7);
    EXPECT_EQ(commands->floor(owner), 2U);
    EXPECT_EQ(commands.find(owner, 2), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 1), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 3), Lookup::unmapped);
    for (const uint64_t kept : {5U, 9U, 7U}) {
        EXPECT_EQ(commands.find(owner, kept), Lookup::retried) << kept;
    }
    commands.insert(owner, 8);
    EXPECT_EQ(commands->floor(owner), 5U);
    EXPECT_EQ(commands.find(owner, 5), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 4), Lookup::expired);
    for (const uint64_t kept : {9U, 7U, 8U}) {
        EXPECT_EQ(commands.find(owner, kept), Lookup::retried) << kept;
    }
}

// ADR-0026 decision 9: the floor never goes down, also when the mapping
// that gives way lies below it.
TEST(CommandTable, floor_never_goes_down) {
    Commands commands(1, 2);
    commands.insert(owner, 10);
    commands.insert(owner, 20);
    commands.insert(owner, 15);
    EXPECT_EQ(commands->floor(owner), 10U);
    commands.insert(owner, 12);
    EXPECT_EQ(commands->floor(owner), 15U);
    // A mapping kept in the row is retried, not expired, below the floor.
    EXPECT_EQ(commands.find(owner, 12), Lookup::retried);
    commands.insert(owner, 16);
    EXPECT_EQ(commands->floor(owner), 15U);
    EXPECT_EQ(commands.find(owner, 12), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 14), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 16), Lookup::retried);
}

// ADR-0026 decision 9: ids at or below the floor are expired; the next one
// is not.
TEST(CommandTable, command_at_the_floor_is_expired_and_one_above_is_not) {
    Commands commands(1, 1);
    commands.insert(owner, 40);
    commands.insert(owner, 50);
    ASSERT_EQ(commands->floor(owner), 40U);
    EXPECT_EQ(commands.find(owner, 40), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 41), Lookup::unmapped);
}

// ADR-0026 decision 9: one principal giving way leaves the other's row and
// floor alone.
TEST(CommandTable, floor_is_per_principal) {
    Commands commands(2, 1);
    commands.insert(owner, 3);
    commands.insert(other, 3);
    commands.insert(owner, 4);
    EXPECT_EQ(commands->floor(owner), 3U);
    EXPECT_EQ(commands->floor(other), 0U);
    EXPECT_EQ(commands.find(owner, 3), Lookup::expired);
    EXPECT_EQ(commands.find(other, 3), Lookup::retried);
    EXPECT_EQ(commands.find(other, 2), Lookup::unmapped);
}

// ADR-0026: the transition that accepted a command journals its mapping,
// found by the transition's commit_seq; a transition that mapped nothing
// finds none.
TEST(CommandTable, mapped_in_names_the_mapping_its_transition_added) {
    Commands commands(1, 2);
    EXPECT_EQ(commands->mapped_in(0), nullptr);
    EXPECT_EQ(commands->mapped_in(1), nullptr);
    commands.insert(owner, 5, 4);
    const CommandMapping *added = commands->mapped_in(4);
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->command_id, 5U);
    EXPECT_EQ(commands->mapped_in(3), nullptr);
    EXPECT_EQ(commands->mapped_in(5), nullptr);

    commands.insert(owner, 6, 5);
    EXPECT_EQ(commands->mapped_in(4), nullptr);
    ASSERT_NE(commands->mapped_in(5), nullptr);
    EXPECT_EQ(commands->mapped_in(5)->command_id, 6U);

    // Giving way moves a mapping in the row; the one added is still found.
    commands.insert(owner, 7, 6);
    const CommandMapping *latest = commands->mapped_in(6);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->command_id, 7U);
    EXPECT_EQ(latest->commit_seq, 6U);
}

// ADR-0026 decision 9: replay maps the command again with its transition's
// commit_seq, and the floor is the larger of the row's and the record's.
TEST(CommandTable, restore_replays_a_mapping_and_keeps_the_larger_floor) {
    Commands commands(1, 1);
    ASSERT_TRUE(commands->restore(mapping(owner, 10, 3), 7));
    EXPECT_EQ(commands.find(owner, 10), Lookup::retried);
    EXPECT_EQ(commands->floor(owner), 7U);
    ASSERT_NE(commands->mapped_in(3), nullptr);
    EXPECT_EQ(commands->mapped_in(3)->command_id, 10U);

    // A record with a lower floor leaves the row's; here giving way raises
    // the row's above the record's.
    ASSERT_TRUE(commands->restore(mapping(owner, 20, 4), 2));
    EXPECT_EQ(commands->floor(owner), 10U);
    ASSERT_TRUE(commands->restore(mapping(owner, 30, 5), 25));
    EXPECT_EQ(commands->floor(owner), 25U);
    EXPECT_EQ(commands.find(owner, 30), Lookup::retried);
    EXPECT_EQ(commands.find(owner, 20), Lookup::expired);
    EXPECT_EQ(commands.find(owner, 26), Lookup::unmapped);
}

// A boot with a smaller capacity gives way to more, so a replayed mapping
// may lie at or below the floor already: it is mapped all the same.
TEST(CommandTable, restore_maps_a_command_below_the_floor) {
    Commands commands(1, 1);
    ASSERT_TRUE(commands->restore(mapping(owner, 10, 3), 0));
    ASSERT_TRUE(commands->restore(mapping(owner, 12, 4), 0));
    ASSERT_EQ(commands->floor(owner), 10U);
    ASSERT_TRUE(commands->restore(mapping(owner, 8, 5), 10));
    EXPECT_EQ(commands.find(owner, 8), Lookup::retried);
    EXPECT_EQ(commands->floor(owner), 12U);
}

// A record the table already holds, or one with no row or no ids, is not
// what a transition writes; restore() refuses it and changes nothing.
TEST(CommandTable, restore_refuses_a_mapping_it_has_or_cannot_hold) {
    Commands commands(1, 2);
    ASSERT_TRUE(commands->restore(mapping(owner, 5, 3), 1));

    CommandMapping conflicting = mapping(owner, 5, 4);
    conflicting.digest += 1U;
    const CommandMapping refused[] = {
        mapping(owner, 5, 4), // same command, same content
        conflicting,          // same command, other content
        mapping(0, 6, 4),     // no principal
        mapping(owner, 0, 4), // no command id
        mapping(other, 6, 4), // no row left
    };
    for (const CommandMapping &record : refused) {
        EXPECT_FALSE(commands->restore(record, 9))
            << record.principal_id << "/" << record.command_id;
    }
    EXPECT_EQ(commands->floor(owner), 1U);
    EXPECT_EQ(commands->floor(other), 0U);
    EXPECT_EQ(commands.find(owner, 5), Lookup::retried);
    EXPECT_EQ(commands.find(owner, 6), Lookup::unmapped);
    EXPECT_EQ(commands.find(other, 6), Lookup::unmapped);
    EXPECT_EQ(commands->mapped_in(4), nullptr);
    ASSERT_NE(commands->mapped_in(3), nullptr);
    EXPECT_EQ(commands->mapped_in(3)->commit_seq, 3U);
}

// A recovery that failed leaves nothing it replayed: rows, floors and the
// mapping to journal are gone.
TEST(CommandTable, clear_empties_every_row) {
    Commands commands(1, 1);
    commands.insert(owner, 5, 2);
    commands.insert(owner, 6, 3);
    ASSERT_EQ(commands.find(owner, 5), Lookup::expired);
    commands->clear();
    EXPECT_EQ(commands.find(owner, 5), Lookup::unmapped);
    EXPECT_EQ(commands.find(owner, 6), Lookup::unmapped);
    EXPECT_EQ(commands->floor(owner), 0U);
    EXPECT_EQ(commands->mapped_in(3), nullptr);
    // The row is free for any principal again.
    EXPECT_TRUE(commands->has_row(other));
    commands.insert(other, 1, 4);
    EXPECT_EQ(commands.find(other, 1), Lookup::retried);
    EXPECT_FALSE(commands->has_row(owner));
}

} // namespace

namespace {

ayni_grant_create_v1 exact_request(uint64_t subject = 1) {
    ayni_grant_create_v1 value{};
    value.struct_size = sizeof(value);
    value.capability_version = 1;
    value.capability_id = {"a", 1};
    value.subject_instance_id = subject;
    value.lifetime = AYNI_GRANT_PERMANENT;
    value.scope.struct_size = sizeof(value.scope);
    return value;
}
CommandMapping exact_mapping(uint64_t command, uint64_t seq) {
    return {
        owner, command, 0, seq, AYNI_INTENT_GRANT_CREATE, {0, command + 500, 1, AYNI_OBJECT_GRANT}};
}

class ExactCommands {
  public:
    ExactCommands(uint32_t capacity = 3, uint32_t bytes = 340, uint32_t rows = 1)
        : memory_(CommandTable::storage_size({rows, capacity, bytes}) / sizeof(uint64_t) + 1),
          table_(memory_.data(), {rows, capacity, bytes}) {}
    CommandTable *operator->() { return &table_; }
    CommandTable::ExactLookup find(uint64_t command, const ayni_grant_create_v1 &request) {
        return table_.find_exact(owner, command, CanonicalCommandView{0, &request});
    }
    void accept(uint64_t command, const ayni_grant_create_v1 &request, uint64_t seq) {
        const auto result = find(command, request);
        ASSERT_EQ(result.status, AYNI_OK);
        ASSERT_EQ(result.lookup, Lookup::unmapped);
        ASSERT_TRUE(table_.insert_exact(exact_mapping(command, seq)));
    }

  private:
    std::vector<uint64_t> memory_;
    CommandTable table_;
};

// C-WIRE-017: accepted bytes are copied, independent of borrowed views and
// whether the created object remains in another module's bounded table.
TEST(CommandTable, exact_retry_returns_the_original_result_and_owns_all_request_bytes) {
    ExactCommands commands;
    auto request = exact_request();
    char id[] = "a";
    request.capability_id = {id, 1};
    commands.accept(5, request, 9);
    id[0] = 'b';
    request = exact_request();
    auto result = commands.find(5, request);
    ASSERT_EQ(result.status, AYNI_OK);
    ASSERT_EQ(result.lookup, Lookup::retried);
    ASSERT_NE(result.mapping, nullptr);
    EXPECT_EQ(result.mapping->object.id, 505U);
    EXPECT_EQ(result.mapping->commit_seq, 9U);
    const auto bytes = commands->canonical(*result.mapping);
    EXPECT_NE(bytes.data, nullptr);
    EXPECT_EQ(bytes.size, result.mapping->canonical_size);
    const auto before = std::vector<uint8_t>(bytes.data, bytes.data + bytes.size);
    request.subject_instance_id = 2;
    EXPECT_EQ(commands.find(5, request).lookup, Lookup::conflict);
    EXPECT_EQ((std::vector<uint8_t>{bytes.data, bytes.data + bytes.size}), before);
    EXPECT_EQ(commands->size(), 1U);
    EXPECT_EQ(commands->floor(owner), 0U);
}

// White-box: force an index collision. Correctness must compare bytes even
// when stored and incoming digest are identical; no fake hash-only equality.
TEST(CommandTable, a_digest_collision_does_not_replace_exact_byte_equality) {
    ExactCommands commands;
    auto first = exact_request(1);
    commands.accept(5, first, 9);
    auto second = exact_request(2);
    std::vector<uint8_t> bytes(128);
    uint32_t size = 0;
    ASSERT_EQ(ayni::kernel::encode_authority_command({0, &second}, bytes.data(),
                                                     static_cast<uint32_t>(bytes.size()), size),
              AYNI_OK);
    auto *mapping = const_cast<CommandMapping *>(commands->mapped_in(9));
    ASSERT_NE(mapping, nullptr);
    mapping->digest = ayni::kernel::authority_command_digest({bytes.data(), size});
    const auto result = commands.find(5, second);
    EXPECT_EQ(result.status, AYNI_OK);
    EXPECT_EQ(result.lookup, Lookup::conflict);
    EXPECT_EQ(result.mapping, nullptr);
    EXPECT_EQ(mapping->object.id, 505U);
}

TEST(CommandTable, exact_payload_moves_with_metadata_and_shares_the_ordinary_window_and_floor) {
    ExactCommands commands;
    commands.accept(5, exact_request(5), 1);
    commands.accept(9, exact_request(9), 2);
    commands.accept(7, exact_request(7), 3);
    commands.accept(8, exact_request(8), 4); // moves last (7) into oldest (5)
    EXPECT_EQ(commands->floor(owner), 5U);
    EXPECT_EQ(commands.find(5, exact_request(5)).lookup, Lookup::expired);
    for (uint64_t id : {7U, 8U, 9U}) {
        const auto result = commands.find(id, exact_request(id));
        ASSERT_EQ(result.lookup, Lookup::retried) << id;
        EXPECT_EQ(result.mapping->object.id, id + 500U);
    }
    commands->insert(mapping(owner, 10, 5)); // same row and floor, not a second window
    EXPECT_EQ(commands->floor(owner), 7U);
    EXPECT_EQ(commands.find(7, exact_request(7)).lookup, Lookup::expired);
    EXPECT_EQ(commands.find(8, exact_request(8)).lookup, Lookup::retried);
    EXPECT_EQ(commands.find(9, exact_request(9)).lookup, Lookup::retried);
    EXPECT_EQ(commands->size(), 3U);
}

TEST(CommandTable,
     ordinary_insert_never_consumes_staged_exact_scratch_and_cross_kind_ids_conflict) {
    ExactCommands commands;
    auto request = exact_request();
    ASSERT_EQ(commands.find(50, request).lookup, Lookup::unmapped);
    commands->insert(mapping(owner, 2, 3));
    EXPECT_FALSE(commands->insert_exact(exact_mapping(50, 4)));
    const auto *ordinary = commands->mapped_in(3);
    ASSERT_NE(ordinary, nullptr);
    EXPECT_EQ(ordinary->canonical_size, 0U);
    EXPECT_EQ(commands->canonical(*ordinary).size, 0U);
    EXPECT_EQ(commands.find(2, request).lookup, Lookup::conflict);
    commands.accept(50, request, 4);
    const CommandMapping *found = nullptr;
    const auto *exact = commands->mapped_in(4);
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(commands->find(owner, 50, exact->digest, found), Lookup::conflict);
    EXPECT_EQ(found, nullptr);
}

TEST(CommandTable,
     malformed_or_oversized_unmapped_requests_change_no_state_and_floor_wins_capacity) {
    ExactCommands commands(1, 57); // exactly the one-character empty request
    auto small = exact_request();
    commands.accept(5, small, 1);
    commands.accept(10, small, 2);
    auto large = small;
    large.capability_id = {"larger", 6};
    EXPECT_EQ(commands.find(5, large).lookup, Lookup::expired);   // before scratch budget
    EXPECT_EQ(commands.find(10, large).lookup, Lookup::conflict); // size before encode
    const auto refused = commands.find(11, large);
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(commands->size(), 1U);
    EXPECT_EQ(commands->floor(owner), 5U);
    auto malformed = small;
    malformed.scope.field_count = 1;
    EXPECT_EQ(commands.find(11, malformed).status, AYNI_INVALID_ARGUMENT);
    EXPECT_FALSE(commands->insert_exact(exact_mapping(11, 3)));
    EXPECT_EQ(commands.find(10, small).lookup, Lookup::retried);
    EXPECT_EQ(commands->mapped_in(3), nullptr);
}

TEST(CommandTable, exact_restore_checks_digest_complete_body_and_current_byte_capacity_atomically) {
    ExactCommands source;
    const auto request = exact_request();
    source.accept(5, request, 9);
    const auto original = *source->mapped_in(9);
    const auto borrowed = source->canonical(*source->mapped_in(9));
    std::vector<uint8_t> bytes(borrowed.data, borrowed.data + borrowed.size);
    const ayni_bytes_v1 body{bytes.data(), static_cast<uint32_t>(bytes.size())};
    ExactCommands too_small(1, body.size - 1);
    EXPECT_EQ(too_small->restore_checked(original, 4, body), CommandTable::Restore::no_room);
    EXPECT_EQ(too_small->size(), 0U);
    EXPECT_EQ(too_small->floor(owner), 0U);
    ExactCommands restored(1);
    auto corrupt = original;
    ++corrupt.digest;
    EXPECT_EQ(restored->restore_checked(corrupt, 4, body), CommandTable::Restore::corrupt);
    EXPECT_EQ(restored->restore_checked(original, 4, {}), CommandTable::Restore::corrupt);
    EXPECT_EQ(restored->size(), 0U);
    EXPECT_EQ(restored->floor(owner), 0U);
    ASSERT_EQ(restored->restore_checked(original, 4, body), CommandTable::Restore::restored);
    bytes[0] = 0; // caller's replay buffer can now be reused
    EXPECT_EQ(restored.find(5, request).lookup, Lookup::retried);
    EXPECT_EQ(restored->floor(owner), 4U);
    EXPECT_EQ(restored->restore_checked(original, 8, body), CommandTable::Restore::corrupt);
    EXPECT_EQ(restored->floor(owner), 4U);
    EXPECT_EQ(restored->size(), 1U);
}

TEST(CommandTable, clear_for_reload_drops_exact_rows_floor_and_staged_candidate) {
    ExactCommands commands(1);
    auto request = exact_request();
    commands.accept(5, request, 1);
    commands.accept(6, request, 2);
    ASSERT_EQ(commands.find(7, request).lookup, Lookup::unmapped);
    commands->clear();
    EXPECT_EQ(commands->floor(owner), 0U);
    EXPECT_EQ(commands->size(), 0U);
    EXPECT_EQ(commands->mapped_in(2), nullptr);
    EXPECT_FALSE(commands->insert_exact(exact_mapping(7, 3)));
    EXPECT_EQ(commands.find(5, request).lookup, Lookup::unmapped);
    EXPECT_EQ(CommandTable::storage_size({2, 3, 340}) - CommandTable::storage_size({2, 3}),
              7U * 340U);
}

TEST(CommandTable, exact_invalid_principal_or_command_cannot_stage_or_change_an_existing_result) {
    ExactCommands commands;
    const auto request = exact_request();
    commands.accept(5, request, 9);
    for (const auto &ids : {std::pair<uint64_t, uint64_t>{0, 6}, {owner, 0}}) {
        const auto result =
            commands->find_exact(ids.first, ids.second, CanonicalCommandView{0, &request});
        EXPECT_EQ(result.status, AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(result.lookup, Lookup::unmapped);
        EXPECT_EQ(result.mapping, nullptr);
        EXPECT_FALSE(commands->insert_exact(exact_mapping(6, 10)));
        EXPECT_EQ(commands->size(), 1U);
        EXPECT_EQ(commands->floor(owner), 0U);
        ASSERT_NE(commands->mapped_in(9), nullptr);
        EXPECT_EQ(commands->mapped_in(9)->object.id, 505U);
    }
}

TEST(CommandTable, staged_identity_cannot_be_consumed_by_another_principal_id_kind_or_bad_result) {
    ExactCommands commands;
    const auto request = exact_request();
    const auto refuses = [&](CommandMapping candidate) {
        ASSERT_EQ(commands.find(5, request).lookup, Lookup::unmapped);
        EXPECT_FALSE(commands->insert_exact(candidate));
        EXPECT_EQ(commands->size(), 0U);
        EXPECT_EQ(commands->floor(owner), 0U);
        EXPECT_EQ(commands->mapped_in(9), nullptr);
    };
    auto candidate = exact_mapping(5, 9);
    candidate.principal_id = other;
    refuses(candidate);
    candidate = exact_mapping(6, 9);
    refuses(candidate);
    candidate = exact_mapping(5, 9);
    candidate.kind = AYNI_INTENT_GOAL_CREATE;
    refuses(candidate);
    candidate = exact_mapping(5, 9);
    candidate.object.revision = 2;
    refuses(candidate);
    commands.accept(5, request, 9);
    ASSERT_EQ(commands->find_exact(other, 6, CanonicalCommandView{0, &request}).lookup,
              Lookup::unmapped);
    candidate = exact_mapping(6, 10);
    candidate.principal_id = other;
    EXPECT_FALSE(commands->insert_exact(candidate)); // The only principal row is retained.
    EXPECT_EQ(commands->size(), 1U);
    EXPECT_EQ(commands.find(5, request).lookup, Lookup::retried);
}

TEST(CommandTable,
     duplicate_exact_restore_keeps_original_payload_and_floor_and_legacy_auth_is_corrupt) {
    ExactCommands source;
    const auto request = exact_request();
    source.accept(5, request, 9);
    const auto mapping = *source->mapped_in(9);
    const auto canonical = source->canonical(*source->mapped_in(9));
    ExactCommands restored;
    ASSERT_EQ(restored->restore_checked(mapping, 4, canonical), CommandTable::Restore::restored);
    EXPECT_EQ(restored->restore_checked(mapping, 8, canonical), CommandTable::Restore::corrupt);
    EXPECT_EQ(restored->floor(owner), 4U);
    EXPECT_EQ(restored->size(), 1U);
    EXPECT_EQ(restored.find(5, request).lookup, Lookup::retried);
    auto legacy = mapping;
    legacy.canonical_size = 0;
    legacy.command_id = 6;
    EXPECT_EQ(restored->restore_checked(legacy, 8), CommandTable::Restore::corrupt);
    EXPECT_EQ(restored->floor(owner), 4U);
    EXPECT_EQ(restored->size(), 1U);
}
// C-WIRE-017 / ADR-0035 decision 8: a failed next lookup ends the old candidate's lifetime.
TEST(CommandTable, failed_exact_lookup_invalidates_the_previous_publishable_candidate) {
    for (const auto invalid_identity : {false, true}) {
        ExactCommands commands;
        auto request = exact_request();
        ASSERT_EQ(commands.find(5, request).lookup, Lookup::unmapped);
        if (invalid_identity) {
            EXPECT_EQ(commands->find_exact(0, 6, CanonicalCommandView{0, &request}).status,
                      AYNI_INVALID_ARGUMENT);
        } else {
            request.lifetime = AYNI_GRANT_CURRENT_BOOT; // Missing required duration.
            EXPECT_EQ(commands.find(6, request).status, AYNI_INVALID_ARGUMENT);
        }
        EXPECT_FALSE(commands->insert_exact(exact_mapping(5, 9)));
        EXPECT_EQ(commands->size(), 0U);
        EXPECT_EQ(commands->floor(owner), 0U);
    }
}

// C-WIRE-004/017: consuming a candidate once cannot publish a second result or evict history.
TEST(CommandTable, a_successfully_consumed_candidate_cannot_publish_again) {
    for (const uint32_t capacity : {1U, 3U}) {
        ExactCommands commands(capacity);
        const auto request = exact_request();
        commands.accept(5, request, 9);
        auto changed = exact_mapping(5, 10);
        ++changed.object.id;
        EXPECT_FALSE(commands->insert_exact(changed));
        EXPECT_EQ(commands->size(), 1U);
        EXPECT_EQ(commands->floor(owner), 0U);
        EXPECT_EQ(commands->mapped_in(10), nullptr);
        const auto retry = commands.find(5, request);
        ASSERT_EQ(retry.lookup, Lookup::retried);
        EXPECT_EQ(retry.mapping->commit_seq, 9U);
        EXPECT_EQ(retry.mapping->object.id, 505U);
    }
}

} // namespace
