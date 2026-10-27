// The persistent store contract (ADR-0051/0059): each family owns its versions,
// REQUIRED boundary, bounds and ordering. The journal and snapshot tests verify bytes.

#include "durability/batch/batch_codec.h"
#include "durability/batch/record_types.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_journal.h"
#include "kernel/service/persistence/work_replay.h"
#include "kernel/service/persistence/work_snapshot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace {
using ayni::durability::RecordHeader;
using ayni::durability::RecordUse;
using ayni::kernel::find_persistent_store;
using ayni::kernel::KernelCapacities;
using ayni::kernel::persistent_store_count;
using ayni::kernel::persistent_stores;
using ayni::kernel::WorkRecordContext;
using ayni::kernel::WorkReplay;
namespace record_type = ayni::durability::record_type;

constexpr uint32_t envelope = static_cast<uint32_t>(ayni::durability::record_header_size);
constexpr WorkRecordContext contexts[] = {WorkRecordContext::journal, WorkRecordContext::snapshot};

// Optional call/Scene stores disabled; mandatory authority/command capacities retained.
KernelCapacities capacities() {
    KernelCapacities capacities{};
    capacities.agent_capacity = 2;
    capacities.goal_capacity = 2;
    capacities.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
    capacities.goal_parameter_max_bytes = 16;
    capacities.task_capacity = 2;
    capacities.policy_intent_capacity = 2;
    capacities.task_step_capacity = 2;
    capacities.task_checkpoint_max_bytes = 8;
    capacities.command_capacity = 2;
    capacities.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    capacities.grant_capacity = 2;
    capacities.grant_delegation_depth_max = 2;
    return capacities;
}

// ADR-0059: one descriptor owns each family; persisted order remains Grant/work/call/Scene/command.
TEST(PersistentStore, every_record_kind_has_one_store_and_a_stable_snapshot_position) {
    using ayni::kernel::StorePosition;
    const uint16_t order[] = {
        record_type::grant,      record_type::calendar,        record_type::call,
        record_type::scene,      record_type::context,         record_type::memory,
        record_type::provenance, record_type::privacy_cleanup, record_type::command_mapping};
    ASSERT_EQ(persistent_store_count, std::size(order));
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        EXPECT_EQ(persistent_stores[rank]->record_type, order[rank]);
        const auto position = rank == 0                            ? StorePosition::before_work
                              : rank + 1 == persistent_store_count ? StorePosition::after_cleanup
                                                                   : StorePosition::after_work;
        EXPECT_EQ(persistent_stores[rank]->position, position);
    }
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        const auto &store = *persistent_stores[rank];
        for (uint32_t kind = 0; kind < store.kind_count; ++kind) {
            const uint16_t type = store.kinds[kind].type;
            EXPECT_NE(std::find(std::begin(record_type::all), std::end(record_type::all), type),
                      std::end(record_type::all));
            uint32_t found = persistent_store_count;
            ASSERT_TRUE(find_persistent_store(type, found));
            EXPECT_EQ(found, rank); // A repeated type would resolve to the earlier owner.
        }
    }
    uint32_t found = 0;
    EXPECT_FALSE(find_persistent_store(record_type::goal, found));
}

// ADR-0059: no storage migration changes unknown-version or legacy optional header semantics.
TEST(PersistentStore, family_versions_keep_their_required_and_context_rules) {
    for (const auto *store : persistent_stores) {
        for (uint32_t kind = 0; kind < store->kind_count; ++kind) {
            const auto type = store->kinds[kind].type;
            for (const auto context : contexts) {
                for (uint16_t version = 0; version < 16U; ++version) {
                    RecordHeader header{type, version, ayni::durability::record_required, 0};
                    const auto classified = store->knows(type, version, context)
                                                ? RecordUse::applied
                                                : RecordUse::unknown;
                    EXPECT_EQ(WorkReplay::classify_header(header, context), classified);
                    header.flags = 0;
                    const bool required = type != record_type::command_mapping || version >= 2U;
                    EXPECT_EQ(WorkReplay::classify_header(header, context),
                              required ? RecordUse::corrupt : classified);
                }
                RecordHeader unknown{type, UINT16_MAX, ayni::durability::record_required, 0};
                EXPECT_EQ(WorkReplay::classify_header(unknown, context), RecordUse::unknown);
                unknown.flags = 0;
                EXPECT_EQ(WorkReplay::classify_header(unknown, context), RecordUse::corrupt);
            }
        }
    }
}

// A disabled store reserves nothing; an enabled one its records per
// transition in a batch, its largest row in a page and every row in a payload.
TEST(PersistentStore, an_enabled_store_adds_exactly_its_share_to_each_bound) {
    const KernelCapacities none = capacities();
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        const auto &store = *persistent_stores[rank];
        if (store.record_type != record_type::call && store.record_type != record_type::scene) {
            continue; // Existing mandatory stores do not have an enable/disable transition.
        }
        ASSERT_EQ(store.capacity(none), 0U);
        KernelCapacities enabled = none;
        if (store.record_type == record_type::call) {
            enabled.call_capacity = 3;
        } else {
            enabled.scene_capacity = 3;
        }
        ASSERT_EQ(store.capacity(enabled), 3U) << rank;
        const uint32_t record = envelope + store.record_max(enabled);
        EXPECT_EQ(ayni::kernel::transition_batch_max(enabled),
                  ayni::kernel::transition_batch_max(none) + store.journal_max(enabled));
        EXPECT_EQ(ayni::kernel::snapshot_payload_max(enabled),
                  ayni::kernel::snapshot_payload_max(none) + uint64_t{3} * record);
        EXPECT_GE(ayni::kernel::snapshot_record_max(enabled), record);
    }
}

} // namespace
