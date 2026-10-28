// ADR-0050: normal public observation facts consumed by Scene prerequisites.
#include "kernel_node.h"
#include "knowledge/observation/observation_table.h"
#include <ayni/observations.h>
#include <gtest/gtest.h>
#include <vector>

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

// ADR-0050/0062: ABI shape and static source type checks are atomic; a
// refused descriptor cannot consume the one available observation slot.
TEST(ObservationApi, invalid_source_shapes_and_semantics_leave_the_registration_slot_unused) {
    Node node;
    node.config().observation_source_capacity = 1;
    node.create();
    struct Case {
        ayni_status_t expected;
        void (*change)(ayni_observation_source_v1 &);
    };
    const Case cases[] = {
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.struct_size = 0; }},
        {AYNI_UNSUPPORTED_VERSION, [](auto &s) { ++s.struct_size; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.source_id = {}; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.unit = {}; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.version = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.kind = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.origin = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.validity_max_us = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.validity_max_us = UINT64_MAX; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.unit = {"mrad", 4}; }},
    };
    auto description = source();
    EXPECT_EQ(ayni_register_observation_source(nullptr, &description), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_observation_source(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    for (const auto &test : cases) {
        description = source();
        test.change(description);
        EXPECT_EQ(ayni_register_observation_source(node.kernel(), &description), test.expected);
    }
    description = source();
    EXPECT_EQ(ayni_register_observation_source(node.kernel(), &description), AYNI_OK);
}

// ADR-0062 / C-SUB-002: rejected Host samples cannot replace the current
// observation. Use a fresh sequence so duplicate suppression cannot mask errors.
TEST(ObservationApi, malformed_samples_preserve_the_last_accepted_identity_and_value) {
    Node node;
    register_source(node);
    node.run();
    auto current = observation(node);
    ASSERT_EQ(ayni_publish_observation(node.kernel(), &current), AYNI_OK);
    struct Case {
        ayni_status_t expected;
        void (*change)(ayni_observation_v1 &);
    };
    const Case cases[] = {
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.struct_size = 0; }},
        {AYNI_UNSUPPORTED_VERSION, [](auto &s) { ++s.struct_size; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.reserved = 1; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.source_id = {}; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.source_version = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.generation = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.sequence = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.valid_for_us = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.valid_for_us = UINT64_MAX; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.quality = 0; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.quality = UINT32_MAX; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.value = -1; }},
        {AYNI_INVALID_ARGUMENT, [](auto &s) { s.value = 2; }},
        {AYNI_UNSUPPORTED_VERSION, [](auto &s) { ++s.source_version; }},
        {AYNI_NOT_FOUND, [](auto &s) { s.source_id = {"absent", 6}; }},
    };
    EXPECT_EQ(ayni_publish_observation(nullptr, &current), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_publish_observation(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    for (const auto &test : cases) {
        auto candidate = current;
        ++candidate.sequence;
        test.change(candidate);
        EXPECT_EQ(ayni_publish_observation(node.kernel(), &candidate), test.expected);
        const auto retained = info(node);
        EXPECT_EQ(retained.reference.sequence, current.sequence);
        EXPECT_EQ(retained.value, current.value);
    }
}

// ADR-0062: null/short worker envelopes fail before retaining an ingress
// item; malformed output buffers retain their caller-owned diagnostics.
TEST(ObservationApi, malformed_worker_envelopes_and_queries_do_not_publish_partial_outputs) {
    Node node;
    register_source(node);
    node.run();
    ayni_observation_post_v1 post{};
    post.struct_size = sizeof(post);
    post.boot_epoch = node.info().boot_epoch;
    post.sample = observation(node);
    EXPECT_EQ(ayni_post_observation(nullptr, &post), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_post_observation(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    const auto original = post;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        auto candidate = original;
        ayni_media_ref_v1 malformed_media{};
        if (scenario == 0) {
            candidate.struct_size = 0;
        }
        if (scenario == 1) {
            ++candidate.struct_size;
        }
        if (scenario == 2) {
            candidate.reserved = 1;
        }
        if (scenario == 3) {
            candidate.boot_epoch = 0;
        }
        if (scenario == 4) {
            candidate.sample.reserved = 1;
        }
        if (scenario == 5) {
            candidate.media = &malformed_media;
        }
        EXPECT_EQ(ayni_post_observation(node.kernel(), &candidate),
                  scenario == 1 ? AYNI_UNSUPPORTED_VERSION : AYNI_INVALID_ARGUMENT);
    }
    ayni_observation_gap_v1 gap{};
    gap.struct_size = sizeof(gap);
    gap.boot_epoch = node.info().boot_epoch;
    gap.source_id = source_id;
    gap.source_version = 1;
    gap.generation = 1;
    EXPECT_EQ(ayni_report_observation_gap(nullptr, &gap), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_report_observation_gap(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        auto candidate = gap;
        if (scenario == 0) {
            candidate.struct_size = 0;
        }
        if (scenario == 1) {
            ++candidate.struct_size;
        }
        if (scenario == 2) {
            candidate.boot_epoch = 0;
        }
        if (scenario == 3) {
            candidate.generation = 0;
        }
        if (scenario == 4) {
            candidate.source_version = 0;
        }
        if (scenario == 5) {
            candidate.source_id = {};
        }
        EXPECT_EQ(ayni_report_observation_gap(node.kernel(), &candidate),
                  scenario == 1 ? AYNI_UNSUPPORTED_VERSION : AYNI_INVALID_ARGUMENT);
    }
    ayni_observation_info_v1 out{};
    out.struct_size = sizeof(out);
    out.reference.sequence = 99;
    EXPECT_EQ(ayni_get_observation(nullptr, source_id, &out), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_observation(node.kernel(), source_id, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_observation(node.kernel(), {}, &out), AYNI_INVALID_ARGUMENT);
    out.struct_size = 0;
    EXPECT_EQ(ayni_get_observation(node.kernel(), source_id, &out), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(out.reference.sequence, 99U);
    ayni_observation_ingress_info_v1 ingress{};
    ingress.struct_size = sizeof(ingress);
    EXPECT_EQ(ayni_get_observation_ingress(nullptr, &ingress), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_observation_ingress(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    ingress.struct_size = 0;
    EXPECT_EQ(ayni_get_observation_ingress(node.kernel(), &ingress), AYNI_INVALID_ARGUMENT);
    ingress.struct_size = sizeof(ingress) + 1;
    EXPECT_EQ(ayni_get_observation_ingress(node.kernel(), &ingress), AYNI_UNSUPPORTED_VERSION);
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
// ADR-0063/C-SUB-002: Scene requirements select the owning Task's scope and
// preserve exact dependency identity; closing one scope cannot validate another.
TEST(ObservationTable, task_scopes_isolate_scene_dependencies_and_closed_refs_stay_invalid) {
    using ayni::catalog::ObservationSource;
    using ayni::catalog::ObservationSources;
    using ayni::knowledge::ObservationTable;
    constexpr uint32_t source_count = 1;
    constexpr uint32_t subscriptions = 2;
    std::vector<uint64_t> registry_bytes(
        ObservationSources::storage_size(source_count) / sizeof(uint64_t) + 1);
    ObservationSources sources(registry_bytes.data(), source_count);
    ObservationSource description{};
    ASSERT_TRUE(description.id.assign(source_id));
    ASSERT_TRUE(description.unit.assign({"bool", 4}));
    description.version = 1;
    description.kind = AYNI_OBSERVATION_BOOL;
    description.origin = AYNI_OBSERVATION_MEASURED;
    description.validity_max_us = 100;
    ASSERT_EQ(sources.add_validated(description), AYNI_OK);
    std::vector<uint64_t> bytes(
        ObservationTable::storage_size(source_count, subscriptions) / sizeof(uint64_t) + 1);
    ObservationTable facts(sources, bytes.data(), source_count, subscriptions);
    facts.begin_boot(1);
    facts.begin_subscription(0, 0, 10, 20, 30);
    facts.begin_subscription(1, 0, 11, 21, 31);
    facts.activate_subscription(0, 30, 100);
    facts.activate_subscription(1, 31, 100);
    ayni::knowledge::ObservationInput sample{};
    sample.source = description.id;
    sample.source_version = 1;
    sample.generation = 30;
    sample.sequence = 1;
    sample.captured_at_us = 1;
    sample.valid_for_us = 100;
    sample.quality = AYNI_OBSERVATION_GOOD;
    sample.value = 1;
    ASSERT_EQ(facts.publish_subscription(sample, 0, 20, 1), AYNI_OK);
    const ayni::catalog::ObservationCondition condition{description.id, 1, 1, 1, 100};
    ayni_observation_ref_v1 a{}, b{};
    ASSERT_EQ(facts.require(condition, 1, a, 10), AYNI_OK);
    EXPECT_EQ(facts.require(condition, 1, b, 11), AYNI_NOT_READY);
    sample.generation = 31;
    ASSERT_EQ(facts.publish_subscription(sample, 1, 21, 1), AYNI_OK);
    ASSERT_EQ(facts.require(condition, 1, b, 11), AYNI_OK);
    EXPECT_NE(a.subscription_id, b.subscription_id);
    EXPECT_TRUE(facts.current(a, 1));
    EXPECT_TRUE(facts.current(b, 1));
    facts.close_subscription(0);
    EXPECT_FALSE(facts.current(a, 1));
    EXPECT_TRUE(facts.current(b, 1));
    EXPECT_EQ(facts.require(condition, 1, a, 10), AYNI_NOT_READY);
    facts.begin_subscription(0, 0, 10, 20, 32);
    facts.activate_subscription(0, 32, 100);
    EXPECT_FALSE(facts.current(a, 1));
}
} // namespace
