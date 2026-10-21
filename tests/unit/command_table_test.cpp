// Management command idempotency in the kernel (C-WIRE-003, C-WIRE-004,
// ADR-0026): what find() makes of a command, the bounded row of each
// principal, the mapping that gives way when a row is full and the floor it
// leaves (ADR-0026 decision 9), the mapping a transition journals, and the
// replay of COMMAND_MAPPING records.

#include "kernel/command/command_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

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
        : storage_(CommandTable::storage_size(rows, capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), rows, capacity) {}

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
