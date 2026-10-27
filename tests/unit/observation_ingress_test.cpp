// ADR-0062: public any-thread sample transport keeps discontinuities reliable
// under FIFO pressure and never turns stale metadata into current knowledge.
#include "work_node.h"
#include <ayni/observations.h>
#include <ayni/providers/mock/media.h>
#include <thread>
namespace {
using namespace ayni::test;
class ObservationIngress : public testing::Test {
  protected:
    Node node;
    uint64_t owner = 0;
    void SetUp() override {
        node.config().observation_source_capacity = 1;
        node.config().observation_ingress_capacity = 2;
        node.on_create([](ayni_kernel_t *kernel) {
            const ayni_observation_source_v1 source{sizeof(source),
                                                    1,
                                                    text("camera"),
                                                    AYNI_OBSERVATION_I64,
                                                    AYNI_OBSERVATION_MEASURED,
                                                    text("count"),
                                                    1000};
            ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
            auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        });
        node.run();
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        ASSERT_EQ(ayni_get_agent(node.kernel(), agent_a, &agent), AYNI_OK);
        owner = agent.instance_id;
    }
    ayni_observation_post_v1 post(uint64_t sequence = 1, uint64_t generation = 1) {
        const ayni_observation_v1 sample{
            sizeof(sample),      1,    text("camera"),        generation, sequence,
            node.sim().now_us(), 1000, AYNI_OBSERVATION_GOOD, 0,          7};
        return {sizeof(ayni_observation_post_v1), 0, node.info().boot_epoch, sample, nullptr};
    }
    ayni_observation_ingress_info_v1 stats() {
        ayni_observation_ingress_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_observation_ingress(node.kernel(), &out), AYNI_OK);
        return out;
    }
    ayni_observation_info_v1 info() {
        ayni_observation_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_observation(node.kernel(), text("camera"), &out), AYNI_OK);
        return out;
    }
    ayni_status_t gap(uint64_t through, uint64_t generation = 1) {
        const ayni_observation_gap_v1 value{sizeof(value),  1,          node.info().boot_epoch,
                                            text("camera"), generation, through};
        return ayni_report_observation_gap(node.kernel(), &value);
    }
    ayni_media_ref_v1 media(uint64_t deadline) {
        ayni_media_ref_v1 value{};
        value.struct_size = sizeof(value);
        value.content_type = AYNI_MEDIA_MOCK_BYTES;
        value.boot_epoch = node.info().boot_epoch;
        value.owner_instance_id = owner;
        value.generation = 1;
        value.byte_size = 3;
        value.valid_until_us = deadline;
        value.source_id.size = 6;
        std::memcpy(value.source_id.data, "camera", 6);
        value.source_version = 1;
        value.purpose.size = 7;
        std::memcpy(value.purpose.data, "inspect", 7);
        return value;
    }
};
TEST_F(ObservationIngress, worker_stack_views_are_copied_and_wake_outside_lock) {
    auto value = post();
    char source[] = "camera";
    value.sample.source_id = {source, 6};
    ayni_status_t status = AYNI_INTERNAL;
    const auto wakes = node.sim().wake_count();
    std::thread worker([&] { status = ayni_post_observation(node.kernel(), &value); });
    worker.join();
    ASSERT_EQ(status, AYNI_OK);
    std::memset(source, 'x', 6);
    value.sample.value = -1;
    EXPECT_EQ(stats().queued, 1U);
    EXPECT_EQ(node.sim().wake_count(), wakes + 1);
    EXPECT_EQ(node.sim().calls_under_lock(), 0U);
    node.step();
    EXPECT_EQ(info().value, 7);
    EXPECT_EQ(info().usable, 1U);
}
TEST_F(ObservationIngress, full_fifo_refuses_samples_but_preserves_gap_and_local_input) {
    auto first = post();
    ASSERT_EQ(ayni_publish_observation(node.kernel(), &first.sample), AYNI_OK);
    auto second = post(2);
    auto third = post(3);
    auto fourth = post(4);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &second), AYNI_OK);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &third), AYNI_OK);
    EXPECT_EQ(ayni_post_observation(node.kernel(), &fourth), AYNI_BUSY);
    EXPECT_EQ(gap(3), AYNI_OK);
    EXPECT_EQ(gap(3), AYNI_OK);
    EXPECT_EQ(stats().gap_updates, 1U);
    EXPECT_EQ(stats().gaps_pending, 1U);
    ayni_event_v1 event{};
    event.struct_size = sizeof(event);
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = text("button");
    EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_OK);
    node.step();
    EXPECT_EQ(info().usable, 0U);
    EXPECT_EQ(info().sequence_gaps, 2U);
    EXPECT_EQ(stats().rejected_busy, 1U);
    EXPECT_EQ(stats().discarded, 2U);
    EXPECT_EQ(stats().gaps_pending, 0U);
    EXPECT_EQ(node.info().events_unmatched, 1U);
    EXPECT_EQ(ayni_post_observation(node.kernel(), &fourth), AYNI_OK);
    node.step();
    EXPECT_EQ(info().reference.sequence, 4U);
    EXPECT_EQ(info().usable, 1U);
    EXPECT_EQ(info().sequence_gaps, 2U);
}
TEST_F(ObservationIngress, later_gaps_count_only_new_losses_and_old_markers_are_idempotent) {
    auto value = post(10);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    EXPECT_EQ(gap(5), AYNI_OK);
    node.step();
    EXPECT_EQ(info().usable, 1U);
    EXPECT_EQ(info().sequence_gaps, 0U);
    value = post(12);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    EXPECT_EQ(info().sequence_gaps, 1U);
    EXPECT_EQ(gap(15), AYNI_OK);
    node.step();
    EXPECT_EQ(info().sequence_gaps, 4U);
    value = post(16);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    EXPECT_EQ(info().sequence_gaps, 4U);
}
TEST_F(ObservationIngress,
       new_generation_gap_refuses_old_tail_and_explicit_zero_boundary_allows_first_sample) {
    auto old = post();
    ASSERT_EQ(ayni_post_observation(node.kernel(), &old), AYNI_OK);
    node.step();
    ASSERT_EQ(gap(0, 2), AYNI_OK);
    auto fresh = post(1, 2);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &old), AYNI_OK);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &fresh), AYNI_OK);
    node.step(1);
    EXPECT_EQ(info().usable, 0U);
    EXPECT_EQ(stats().discarded, 1U);
    EXPECT_EQ(stats().queued, 1U);
    node.step(1);
    EXPECT_EQ(info().reference.generation, 2U);
    EXPECT_EQ(info().usable, 1U);
}
TEST_F(ObservationIngress, owner_discards_expired_or_future_samples_and_respects_item_budget) {
    auto expired = post();
    auto future = post(2);
    future.sample.captured_at_us += 2000;
    ASSERT_EQ(ayni_post_observation(node.kernel(), &expired), AYNI_OK);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &future), AYNI_OK);
    node.sim().advance_us(1000);
    const auto report = node.step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(stats().queued, 1U);
    node.step(1);
    EXPECT_EQ(stats().discarded, 2U);
    ayni_observation_info_v1 empty{};
    empty.struct_size = sizeof(empty);
    EXPECT_EQ(ayni_get_observation(node.kernel(), text("camera"), &empty), AYNI_NOT_FOUND);
}
TEST_F(ObservationIngress, close_discards_accepted_samples_and_orderly_reopen_rejects_old_boot) {
    auto old = post();
    ASSERT_EQ(ayni_post_observation(node.kernel(), &old), AYNI_OK);
    ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    EXPECT_EQ(stats().discarded, 1U);
    EXPECT_EQ(stats().queued, 0U);
    EXPECT_EQ(ayni_post_observation(node.kernel(), &old), AYNI_NOT_READY);
    node.reboot();
    node.recover();
    EXPECT_NE(node.info().boot_epoch, old.boot_epoch);
    EXPECT_EQ(ayni_post_observation(node.kernel(), &old), AYNI_STALE);
    auto current = post();
    EXPECT_EQ(ayni_post_observation(node.kernel(), &current), AYNI_OK);
    node.step();
    EXPECT_EQ(info().reference.boot_epoch, current.boot_epoch);
}
TEST_F(ObservationIngress, nested_shape_and_static_source_failures_preserve_queue) {
    auto value = post();
    value.sample.source_version = 2;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_UNSUPPORTED_VERSION);
    value = post();
    ++value.sample.struct_size;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_UNSUPPORTED_VERSION);
    value = post();
    value.sample.valid_for_us = 1001;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_INVALID_ARGUMENT);
    value = post();
    value.boot_epoch += 1;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_STALE);
    EXPECT_EQ(stats().accepted, 0U);
    EXPECT_EQ(node.sim().calls_under_lock(), 0U);
}
TEST_F(ObservationIngress, media_metadata_is_copied_and_expires_with_its_shorter_lifetime) {
    ayni::providers::mock::MediaStore store;
    ASSERT_EQ(store.configure({1, 3}), AYNI_OK);
    ASSERT_EQ(store.begin_boot(node.info().boot_epoch), AYNI_OK);
    const uint8_t bytes[]{3, 4, 5};
    auto description = media(node.sim().now_us() + 100);
    ayni_media_ref_v1 reference{};
    ASSERT_EQ(store.put(description, {bytes, sizeof(bytes)}, node.sim().now_us(), reference),
              AYNI_OK);
    auto value = post();
    value.media = &reference;
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    const auto id = reference.media_id;
    reference.media_id = 0;
    node.step();
    EXPECT_EQ(info().media.media_id, id);
    EXPECT_EQ(info().usable, 1U);
    node.sim().advance_us(100);
    EXPECT_EQ(info().usable, 0U);
    store.expire(node.sim().now_us());
    EXPECT_EQ(store.occupied(), 0U);
}
TEST_F(ObservationIngress, media_owner_and_generation_cannot_be_fabricated_by_late_samples) {
    auto reference = media(node.sim().now_us() + 100);
    reference.media_id = 1;
    auto value = post();
    value.media = &reference;
    ++reference.generation;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_INVALID_ARGUMENT);
    --reference.generation;
    reference.owner_instance_id = UINT64_MAX;
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    EXPECT_EQ(stats().discarded, 1U);
    reference.owner_instance_id = owner;
    ++reference.boot_epoch;
    EXPECT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_STALE);
}
// C-OBS-003: a late gap cannot recount losses from an older incarnation.
TEST_F(ObservationIngress, old_generation_gap_does_not_recount_historical_losses) {
    auto value = post();
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    value = post(4);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    EXPECT_EQ(info().sequence_gaps, 2U);
    value = post(1, 2);
    ASSERT_EQ(ayni_post_observation(node.kernel(), &value), AYNI_OK);
    node.step();
    ASSERT_EQ(gap(4), AYNI_OK);
    node.step();
    EXPECT_EQ(info().usable, 1U);
    EXPECT_EQ(info().sequence_gaps, 2U);
}
// C-OBS-002: source configuration selects independent, explicit bounded ingress.
TEST(ObservationIngressConfig,
     disabled_sources_reject_queue_and_enabled_sources_default_to_sixteen) {
    Node disabled;
    disabled.run();
    ayni_observation_post_v1 value{sizeof(value),
                                   0,
                                   disabled.info().boot_epoch,
                                   {sizeof(ayni_observation_v1), 1, text("camera"), 1, 1,
                                    disabled.sim().now_us(), 100, AYNI_OBSERVATION_GOOD, 0, 0},
                                   nullptr};
    EXPECT_EQ(ayni_post_observation(disabled.kernel(), &value), AYNI_UNSUPPORTED);
    auto bad = disabled.config();
    bad.observation_ingress_capacity = 1;
    std::size_t bytes = 0, alignment = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(&bad, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
    Node enabled;
    enabled.config().observation_source_capacity = 1;
    enabled.run();
    ayni_observation_ingress_info_v1 stats{};
    stats.struct_size = sizeof(stats);
    ASSERT_EQ(ayni_get_observation_ingress(enabled.kernel(), &stats), AYNI_OK);
    EXPECT_EQ(stats.capacity, AYNI_OBSERVATION_INGRESS_CAPACITY_DEFAULT);
    bad = enabled.config();
    bad.observation_ingress_capacity = AYNI_OBSERVATION_INGRESS_CAPACITY_MAX + 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&bad, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
}
} // namespace
