// ADR-0050: normal public observation facts consumed by Scene prerequisites.
#include "kernel_node.h"
#include <ayni/observations.h>
#include <gtest/gtest.h>

namespace {
using ayni::test::Node;
const ayni_string_v1 source_id{"ready", 5};
ayni_observation_source_v1 source() {
    return {sizeof(ayni_observation_source_v1), 1,           source_id, AYNI_OBSERVATION_BOOL,
            AYNI_OBSERVATION_MEASURED,          {"bool", 4}, 100};
}
ayni_observation_v1 observation(Node &node) {
    return {sizeof(ayni_observation_v1), 1, source_id, 1, 1, node.sim().now_us(), 100,
            AYNI_OBSERVATION_GOOD,       0, 1};
}
ayni_observation_info_v1 info(Node &node) {
    ayni_observation_info_v1 out{};
    out.struct_size = sizeof(out);
    EXPECT_EQ(ayni_get_observation(node.kernel(), source_id, &out), AYNI_OK);
    return out;
}
void register_source(Node &node, ayni_observation_source_v1 description = source()) {
    node.config().observation_source_capacity = 1;
    node.on_create([description](ayni_kernel_t *kernel) {
        ASSERT_EQ(ayni_register_observation_source(kernel, &description), AYNI_OK);
    });
}

TEST(ObservationApi, copied_source_and_explicit_capacity_precede_publication) {
    Node node;
    node.config().observation_source_capacity = 1;
    node.create();
    auto description = source();
    char local_id[] = "ready";
    description.source_id = {local_id, 5};
    ASSERT_EQ(ayni_register_observation_source(node.kernel(), &description), AYNI_OK);
    local_id[0] = 'x';
    description.source_id = {"other", 5};
    EXPECT_EQ(ayni_register_observation_source(node.kernel(), &description),
              AYNI_CAPACITY_EXCEEDED);
    auto sample = observation(node);
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_NOT_READY);
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    node.recover();
    sample = observation(node);
    const auto wakes = node.sim().wake_count();
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    EXPECT_EQ(node.sim().wake_count(), wakes + 1);
    EXPECT_EQ(info(node).reference.source_id.data[0], 'r');
    EXPECT_EQ(info(node).value, 1);
    EXPECT_EQ(ayni_register_observation_source(node.kernel(), &description), AYNI_INVALID_STATE);
}

TEST(ObservationApi, expiration_is_visible_without_erasing_the_last_fact) {
    Node node;
    register_source(node);
    node.run();
    auto sample = observation(node);
    ASSERT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    EXPECT_EQ(info(node).usable, 1U);
    node.sim().advance_us(100);
    EXPECT_EQ(info(node).usable, 0U);
    EXPECT_EQ(info(node).reference.sequence, 1U);
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_EXPIRED);
    ++sample.sequence;
    sample.captured_at_us = node.sim().now_us();
    sample.quality = AYNI_OBSERVATION_DEGRADED;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    EXPECT_EQ(info(node).usable, 0U);
    EXPECT_EQ(info(node).quality, AYNI_OBSERVATION_DEGRADED);
}

TEST(ObservationApi, sequence_generation_and_type_boundaries_preserve_the_last_accepted_sample) {
    Node node;
    register_source(node);
    node.run();
    auto sample = observation(node);
    ASSERT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_STALE);
    ++sample.generation;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    --sample.generation;
    ++sample.sequence;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_STALE);
    ++sample.generation;
    sample.value = 2;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_INVALID_ARGUMENT);
    sample.value = 0;
    sample.valid_for_us = 101;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_INVALID_ARGUMENT);
    sample.valid_for_us = 100;
    ++sample.captured_at_us;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info(node).reference.generation, 2U);
    EXPECT_EQ(info(node).reference.sequence, 1U);
    EXPECT_EQ(info(node).value, 1);
}

TEST(ObservationApi, inferred_source_remains_explicit_and_missing_query_preserves_output) {
    Node node;
    auto description = source();
    description.origin = AYNI_OBSERVATION_INFERRED;
    register_source(node, description);
    node.run();
    auto sample = observation(node);
    ASSERT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_OK);
    auto out = info(node);
    EXPECT_EQ(out.origin, AYNI_OBSERVATION_INFERRED);
    EXPECT_EQ(out.usable, 1U); // General data validity is distinct from Scene admissibility.
    EXPECT_EQ(ayni_get_observation(node.kernel(), {"missing", 7}, &out), AYNI_NOT_FOUND);
    EXPECT_EQ(out.reference.sequence, 1U);
    ++out.struct_size;
    EXPECT_EQ(ayni_get_observation(node.kernel(), source_id, &out), AYNI_UNSUPPORTED_VERSION);
}
// ADR-0050/0062: adding async transport must preserve the synchronous ABI's
// classified error order even when a sample has several invalid conditions.
TEST(ObservationApi, source_failures_precede_expiry_but_future_time_precedes_source) {
    Node node;
    register_source(node);
    node.run();
    auto sample = observation(node);
    node.sim().advance_us(100);
    sample.source_id = {"missing", 7};
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_NOT_FOUND);
    sample.source_id = source_id;
    ++sample.source_version;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_UNSUPPORTED_VERSION);
    --sample.source_version;
    sample.value = 2;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_INVALID_ARGUMENT);
    sample.value = 1;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_EXPIRED);
    sample.source_id = {"missing", 7};
    sample.captured_at_us = node.sim().now_us() + 1;
    EXPECT_EQ(ayni_publish_observation(node.kernel(), &sample), AYNI_INVALID_ARGUMENT);
}
} // namespace
