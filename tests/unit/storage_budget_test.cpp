// White-box: the log limit and the free-space reserves a RUNNING kernel
// works within (C-STORE-011, ADR-0027 decision 9). The expected
// figures are written out here from what storage_budget.h states, not taken
// from StorageBudget, so a changed formula fails.

#include "durability/batch/batch_codec.h"
#include "kernel/kernel_config.h"
#include "kernel/storage/storage_budget.h"
#include "kernel/work_journal.h"
#include "kernel/work_snapshot.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelLimits;
using ayni::kernel::StorageBudget;

// What the fake Host reports as free.
struct FreeSpace {
    ayni_status_t status = AYNI_OK;
    uint64_t bytes = UINT64_MAX;
};

ayni_status_t free_bytes(void *ctx, uint64_t *out_bytes) {
    const FreeSpace &space = *static_cast<FreeSpace *>(ctx);
    *out_bytes = space.bytes;
    return space.status;
}

ayni_host_v1 host_of(FreeSpace &space) {
    ayni_host_v1 host{};
    host.struct_size = sizeof(host);
    host.ctx = &space;
    host.storage_free_bytes = &free_bytes;
    return host;
}

// Many objects and a buffer that just holds a batch, so the snapshot
// reserve is the larger one.
KernelCapacities large_model() {
    KernelCapacities capacities{};
    capacities.agent_capacity = 8;
    capacities.goal_type_capacity = 4;
    capacities.goal_capacity = 64;
    capacities.goal_parameter_max_bytes = 64;
    capacities.session_capacity = 2;
    capacities.task_capacity = 16;
    capacities.strategy_capacity = 2;
    capacities.policy_intent_capacity = 2;
    capacities.task_step_capacity = 4;
    capacities.task_checkpoint_max_bytes = 64;
    capacities.command_capacity = 8;
    capacities.storage_buffer_bytes = ayni::kernel::storage_buffer_min(capacities);
    return capacities;
}

// Few objects and the largest buffer, so the log reserve is the larger one.
KernelCapacities large_buffer() {
    KernelCapacities capacities = large_model();
    capacities.goal_capacity = 2;
    capacities.task_capacity = 1;
    capacities.command_capacity = 1;
    capacities.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_MAX;
    return capacities;
}

uint64_t batch_max(const KernelCapacities &capacities) {
    return ayni::kernel::transition_batch_max(capacities);
}

// SNAPSHOT_COMMIT: one 32-byte record in a batch of its own (records.md).
uint64_t activation_batch() {
    return ayni::durability::batch_size(32, 1);
}

// The journal buffer, one more batch and a 1024-byte margin.
uint64_t log_reserve(const KernelCapacities &capacities) {
    return uint64_t{capacities.storage_buffer_bytes} + batch_max(capacities) + 1024U;
}

// The largest payload with its 72-byte header and 24-byte marker, the
// activation batch and two of the largest batches.
uint64_t snapshot_reserve(const KernelCapacities &capacities) {
    return ayni::kernel::snapshot_payload_max(capacities) + 72U + 24U + activation_batch() +
           2U * batch_max(capacities);
}

KernelLimits limits_with(uint32_t log_max_bytes) {
    KernelLimits limits{};
    limits.storage_log_max_bytes = log_max_bytes;
    return limits;
}

// The limit ayni_create resolves storage_log_max_bytes 0 to.
KernelLimits default_limits(const KernelCapacities &capacities) {
    return limits_with(StorageBudget::log_max_default(capacities.storage_buffer_bytes));
}

TEST(StorageBudget, default_log_limit_is_eight_buffers_or_one_mib) {
    EXPECT_EQ(StorageBudget::log_max_default(AYNI_STORAGE_BUFFER_BYTES_MIN), 1048576U);
    EXPECT_EQ(StorageBudget::log_max_default(131072), 1048576U);
    EXPECT_EQ(StorageBudget::log_max_default(131073), 8U * 131073U);
    EXPECT_EQ(StorageBudget::log_max_default(AYNI_STORAGE_BUFFER_BYTES_MAX),
              8U * AYNI_STORAGE_BUFFER_BYTES_MAX);
}

TEST(StorageBudget, smallest_log_limit_holds_the_reserve_two_batches_and_an_activation) {
    for (const KernelCapacities &capacities : {large_model(), large_buffer()}) {
        EXPECT_EQ(StorageBudget::log_max_min(capacities),
                  log_reserve(capacities) + 2U * batch_max(capacities) + activation_batch());
    }
}

TEST(StorageBudget, admission_needs_the_larger_reserve_free) {
    const KernelCapacities model = large_model();
    ASSERT_GT(snapshot_reserve(model), log_reserve(model));
    const KernelCapacities buffer = large_buffer();
    ASSERT_GT(log_reserve(buffer), snapshot_reserve(buffer));
    for (const KernelCapacities &capacities : {model, buffer}) {
        const uint64_t required = snapshot_reserve(capacities) > log_reserve(capacities)
                                      ? snapshot_reserve(capacities)
                                      : log_reserve(capacities);
        const StorageBudget budget(capacities, default_limits(capacities));
        FreeSpace space;
        const ayni_host_v1 host = host_of(space);
        space.bytes = required - 1U;
        EXPECT_EQ(budget.admission(host), AYNI_STORAGE_FAILED);
        space.bytes = required;
        EXPECT_EQ(budget.admission(host), AYNI_OK);
        // A Host that cannot tell admits nothing either.
        space.status = AYNI_STORAGE_FAILED;
        EXPECT_EQ(budget.admission(host), AYNI_STORAGE_FAILED);
    }
    ayni_host_v1 none{};
    none.struct_size = sizeof(none);
    EXPECT_EQ(StorageBudget(model, default_limits(model)).admission(none), AYNI_STORAGE_FAILED);
}

TEST(StorageBudget, compaction_is_due_at_three_quarters_of_the_limit) {
    const KernelCapacities capacities = large_model();
    const uint32_t limit = 1048576;
    ASSERT_LT(log_reserve(capacities), limit / 4U);
    StorageBudget budget(capacities, limits_with(limit));
    FreeSpace space;
    const ayni_host_v1 host = host_of(space);
    budget.restart(limit / 4U * 3U - 1U);
    budget.refresh(host);
    EXPECT_FALSE(budget.compaction_due());
    EXPECT_TRUE(budget.admits());
    budget.appended(1);
    budget.refresh(host);
    EXPECT_TRUE(budget.compaction_due());
    EXPECT_FALSE(budget.admits());
    // The request stays until the log is compacted.
    budget.refresh(host);
    EXPECT_TRUE(budget.compaction_due());
    budget.compacted(activation_batch());
    EXPECT_FALSE(budget.compaction_due());
    budget.refresh(host);
    EXPECT_FALSE(budget.compaction_due());
    EXPECT_TRUE(budget.admits());
}

TEST(StorageBudget, compaction_is_due_before_the_log_reserve_would_cross_the_limit) {
    const KernelCapacities capacities = large_model();
    // A limit whose reserve reaches below three quarters of it.
    const uint64_t limit = 2U * log_reserve(capacities);
    StorageBudget budget(capacities, limits_with(static_cast<uint32_t>(limit)));
    FreeSpace space;
    const ayni_host_v1 host = host_of(space);
    budget.restart(limit - log_reserve(capacities) - 1U);
    budget.refresh(host);
    EXPECT_FALSE(budget.compaction_due());
    budget.appended(1);
    budget.refresh(host);
    EXPECT_TRUE(budget.compaction_due());
    // A reload drops the request, and so does a restart.
    budget.cancel_compaction();
    EXPECT_FALSE(budget.compaction_due());
    budget.refresh(host);
    EXPECT_TRUE(budget.compaction_due());
    budget.restart(0);
    EXPECT_FALSE(budget.compaction_due());
}

TEST(StorageBudget, a_space_shortage_holds_transitions_until_a_refresh_finds_room) {
    const KernelCapacities capacities = large_model();
    StorageBudget budget(capacities, default_limits(capacities));
    FreeSpace space;
    const ayni_host_v1 host = host_of(space);
    budget.refresh(host);
    EXPECT_TRUE(budget.admits());
    space.bytes = 0;
    budget.refresh(host);
    EXPECT_FALSE(budget.admits());
    EXPECT_FALSE(budget.compaction_due());
    space.bytes = UINT64_MAX;
    budget.refresh(host);
    EXPECT_TRUE(budget.admits());
}

TEST(StorageBudget, a_snapshot_needs_its_reserve_and_a_shortage_holds_transitions) {
    const KernelCapacities capacities = large_model();
    StorageBudget budget(capacities, default_limits(capacities));
    FreeSpace space;
    const ayni_host_v1 host = host_of(space);
    budget.refresh(host);
    ASSERT_TRUE(budget.admits());
    space.bytes = snapshot_reserve(capacities);
    EXPECT_TRUE(budget.refresh_for_snapshot(host));
    EXPECT_TRUE(budget.admits());
    space.bytes = snapshot_reserve(capacities) - 1U;
    EXPECT_FALSE(budget.refresh_for_snapshot(host));
    EXPECT_FALSE(budget.admits());
    space.bytes = UINT64_MAX;
    budget.refresh(host);
    EXPECT_TRUE(budget.admits());
    space.status = AYNI_STORAGE_FAILED;
    EXPECT_FALSE(budget.refresh_for_snapshot(host));
}

} // namespace
