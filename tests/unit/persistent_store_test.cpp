// The persistent store contract (ADR-0051 decision 8): each store writes one
// type of the record registry, its header is REQUIRED in the log and the
// snapshot alike, and the journal and snapshot bounds grow by exactly the
// share the store declares once it is enabled. The journal, snapshot and
// restore tests check the bytes themselves.

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

// Every store disabled; each test enables the one it measures.
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

// records.md: calls follow the Tasks they name, Scenes follow calls.
TEST(PersistentStore, each_store_writes_one_registered_type_in_log_order) {
    ASSERT_EQ(persistent_store_count, 2U);
    EXPECT_EQ(persistent_stores[0]->record_type, record_type::call);
    EXPECT_EQ(persistent_stores[1]->record_type, record_type::scene);
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        const uint16_t type = persistent_stores[rank]->record_type;
        EXPECT_NE(std::find(std::begin(record_type::all), std::end(record_type::all), type),
                  std::end(record_type::all));
        uint32_t found = persistent_store_count;
        ASSERT_TRUE(find_persistent_store(type, found));
        EXPECT_EQ(found, rank);
    }
    uint32_t found = 0;
    EXPECT_FALSE(find_persistent_store(record_type::goal, found));
    EXPECT_FALSE(find_persistent_store(record_type::command_mapping, found));
}

TEST(PersistentStore, a_store_header_must_be_required_whether_or_not_its_version_is_known) {
    for (const auto *store : persistent_stores) {
        for (const auto context : contexts) {
            uint16_t known = 0;
            for (uint16_t version = 1; version < 64U && known == 0U; ++version) {
                known = store->knows(version, context) ? version : 0U;
            }
            ASSERT_NE(known, 0U) << store->record_type;
            RecordHeader header{store->record_type, known, ayni::durability::record_required, 0};
            EXPECT_EQ(WorkReplay::classify_header(header, context), RecordUse::applied);
            header.flags = 0;
            EXPECT_EQ(WorkReplay::classify_header(header, context), RecordUse::corrupt);
            header.version = UINT16_MAX;
            EXPECT_EQ(WorkReplay::classify_header(header, context), RecordUse::corrupt);
            header.flags = ayni::durability::record_required;
            EXPECT_EQ(WorkReplay::classify_header(header, context), RecordUse::unknown);
        }
    }
}

// A disabled store reserves nothing; an enabled one its records per
// transition in a batch, its largest row in a page and every row in a payload.
TEST(PersistentStore, an_enabled_store_adds_exactly_its_share_to_each_bound) {
    const KernelCapacities none = capacities();
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        const auto &store = *persistent_stores[rank];
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
                  ayni::kernel::transition_batch_max(none) + store.records_per_transition * record);
        EXPECT_EQ(ayni::kernel::snapshot_payload_max(enabled),
                  ayni::kernel::snapshot_payload_max(none) + uint64_t{3} * record);
        EXPECT_GE(ayni::kernel::snapshot_record_max(enabled), record);
    }
}

} // namespace
