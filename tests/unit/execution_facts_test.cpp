// White-box, ADR-0050 / C-STORE-007/009: execution replay retains the original
// permit envelope, observation identities and independent stop measurement.
#include "foundation/kernel_id.h"
#include "work/execution/execution_fact.h"
#include <ayni/providers/mock_execution.h>

#include <cstring>
#include <gtest/gtest.h>
#include <iterator>
#include <vector>

namespace {
using namespace ayni::work;
ayni::catalog::CatalogId token(const char *text) {
    ayni::catalog::CatalogId result;
    EXPECT_TRUE(result.assign({text, static_cast<uint32_t>(std::strlen(text))}));
    return result;
}
ExecutionFact execution_fact() {
    ExecutionFact fact{};
    fact.binding.binding = token("motion");
    fact.binding.binding_version = 1;
    fact.binding.executor = token("local");
    fact.binding.executor_version = 1;
    fact.binding.scene = token("look");
    fact.binding.scene_version = 1;
    fact.binding.form = AYNI_FORM_PROCEDURE;
    fact.binding.lease_us = 100000;
    fact.binding.max_command_age_us = 1000;
    fact.binding.field_count = 1;
    fact.binding.fields[0] = {token("angle"), token("degree"), 1, 2};
    fact.scene_seq = 1;
    fact.control_domain = 1;
    fact.control_mode = 1;
    fact.epochs = {1, 1, 1, 1, 1, 0};
    fact.dependency_count = 1;
    auto &ref = fact.dependencies[0];
    ref.source_id = token("ready").snapshot();
    ref.source_version = 1;
    ref.boot_epoch = 1;
    ref.generation = 1;
    ref.sequence = 1;
    return fact;
}

TEST(ExecutionFacts, invalid_metadata_cannot_authorize_recovered_physical_execution) {
    const auto original = execution_fact();
    ASSERT_TRUE(valid_execution_metadata(original));
    struct Case {
        const char *name;
        void (*change)(ExecutionFact &);
    };
    const Case cases[] = {
        {"empty binding", [](auto &f) { f.binding.binding = {}; }},
        {"zero binding version", [](auto &f) { f.binding.binding_version = 0; }},
        {"empty executor", [](auto &f) { f.binding.executor = {}; }},
        {"zero executor version", [](auto &f) { f.binding.executor_version = 0; }},
        {"empty scene", [](auto &f) { f.binding.scene = {}; }},
        {"zero scene version", [](auto &f) { f.binding.scene_version = 0; }},
        {"unknown execution form", [](auto &f) { f.binding.form = 0; }},
        {"zero lease", [](auto &f) { f.binding.lease_us = 0; }},
        {"zero command age", [](auto &f) { f.binding.max_command_age_us = 0; }},
        {"empty parameter mapping", [](auto &f) { f.binding.field_count = 0; }},
        {"oversized parameter mapping",
         [](auto &f) { f.binding.field_count = AYNI_CONTROL_FIELD_COUNT_MAX + 1; }},
        {"empty parameter name", [](auto &f) { f.binding.fields[0].id = {}; }},
        {"empty parameter unit", [](auto &f) { f.binding.fields[0].unit = {}; }},
        {"missing minimum source", [](auto &f) { f.binding.fields[0].minimum_field = 0; }},
        {"missing maximum source", [](auto &f) { f.binding.fields[0].maximum_field = 0; }},
        {"duplicate parameter mapping",
         [](auto &f) {
             f.binding.fields[1] = f.binding.fields[0];
             f.binding.field_count = 2;
         }},
        {"missing scene acceptance", [](auto &f) { f.scene_seq = 0; }},
        {"missing control domain", [](auto &f) { f.control_domain = 0; }},
        {"missing control mode", [](auto &f) { f.control_mode = 0; }},
        {"missing kernel epoch", [](auto &f) { f.epochs.kernel_boot_epoch = 0; }},
        {"missing authority epoch", [](auto &f) { f.epochs.authority_epoch = 0; }},
        {"missing executor epoch", [](auto &f) { f.epochs.executor_boot_epoch = 0; }},
        {"missing safety epoch", [](auto &f) { f.epochs.safety_epoch = 0; }},
        {"missing calibration", [](auto &f) { f.epochs.calibration_version = 0; }},
        {"reserved epoch bits", [](auto &f) { f.epochs.reserved = 1; }},
        {"phase below range", [](auto &f) { f.phase = 0; }},
        {"phase above range", [](auto &f) { f.phase = UINT32_MAX; }},
        {"unknown measured target", [](auto &f) { f.target = UINT32_MAX; }},
        {"unknown stop cause", [](auto &f) { f.stop_cause = UINT32_MAX; }},
        {"stopped without stop proof", [](auto &f) { f.phase = AYNI_EXECUTION_STOPPED; }},
        {"prepared with stop proof", [](auto &f) { f.stop_cause = AYNI_CONTROL_STOP_REQUESTED; }},
        {"prepared with measured target", [](auto &f) { f.target = AYNI_TARGET_SATISFIED; }},
        {"too many observations",
         [](auto &f) { f.dependency_count = AYNI_OBSERVATION_CONDITION_COUNT_MAX + 1; }},
        {"oversized source identity",
         [](auto &f) { f.dependencies[0].source_id.size = AYNI_CATALOG_ID_MAX_BYTES + 1; }},
        {"empty source identity", [](auto &f) { f.dependencies[0].source_id = {}; }},
        {"zero source version", [](auto &f) { f.dependencies[0].source_version = 0; }},
        {"different observation boot", [](auto &f) { ++f.dependencies[0].boot_epoch; }},
        {"invalid subscription identity", [](auto &f) { f.dependencies[0].subscription_id = 1; }},
        {"future subscription identity",
         [](auto &f) {
             ayni::foundation::IdAllocator ids;
             ids.start(2);
             EXPECT_TRUE(ids.allocate(f.dependencies[0].subscription_id));
         }},
        {"zero observation generation", [](auto &f) { f.dependencies[0].generation = 0; }},
        {"zero observation sequence", [](auto &f) { f.dependencies[0].sequence = 0; }},
        {"duplicate observation source",
         [](auto &f) {
             f.dependencies[1] = f.dependencies[0];
             f.dependency_count = 2;
         }},
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        auto candidate = original;
        test.change(candidate);
        EXPECT_FALSE(valid_execution_metadata(candidate));
    }
}

TEST(ExecutionFacts, stopped_execution_cannot_reopen_or_rewrite_its_measurement) {
    auto prepared = execution_fact();
    auto dispatched = prepared;
    dispatched.phase = AYNI_EXECUTION_DISPATCHING;
    auto stopped = dispatched;
    stopped.phase = AYNI_EXECUTION_STOPPED;
    stopped.stop_cause = AYNI_CONTROL_STOP_REQUESTED;
    auto never_started = prepared;
    never_started.phase = AYNI_EXECUTION_NOT_STARTED;
    const ExecutionFact states[] = {prepared, dispatched, stopped, never_started};
    // Rows/columns: PREPARED, DISPATCHING, STOPPED, NOT_STARTED.
    constexpr bool allowed[][4] = {{true, true, true, true},
                                   {false, true, true, true},
                                   {false, false, true, false},
                                   {false, false, false, true}};
    for (std::size_t from = 0; from < std::size(states); ++from) {
        ASSERT_TRUE(valid_execution_metadata(states[from]));
        for (std::size_t to = 0; to < std::size(states); ++to) {
            EXPECT_EQ(execution_advances(states[from], states[to]), allowed[from][to])
                << from << ',' << to;
        }
    }
    auto changed = stopped;
    changed.target = AYNI_TARGET_SATISFIED;
    ASSERT_TRUE(valid_execution_metadata(changed));
    EXPECT_FALSE(execution_advances(stopped, changed));
    changed = stopped;
    changed.stop_cause = AYNI_CONTROL_STOP_EXPIRED;
    EXPECT_FALSE(execution_advances(stopped, changed));
    EXPECT_TRUE(execution_advances(std::nullopt, std::nullopt));
    EXPECT_FALSE(execution_advances(prepared, std::nullopt));
    EXPECT_FALSE(execution_advances(std::nullopt, prepared));
}

TEST(ExecutionFacts, complete_identity_includes_each_original_epoch_and_observation_reference) {
    const auto original = execution_fact();
    const auto changes = {
        +[](ExecutionFact &f) { ++f.binding.binding_version; },
        +[](ExecutionFact &f) { ++f.binding.executor_version; },
        +[](ExecutionFact &f) { ++f.binding.scene_version; },
        +[](ExecutionFact &f) { ++f.scene_seq; },
        +[](ExecutionFact &f) { ++f.control_domain; },
        +[](ExecutionFact &f) { ++f.control_mode; },
        +[](ExecutionFact &f) {
            ++f.epochs.kernel_boot_epoch;
            ++f.dependencies[0].boot_epoch;
        },
        +[](ExecutionFact &f) { ++f.epochs.authority_epoch; },
        +[](ExecutionFact &f) { ++f.epochs.executor_boot_epoch; },
        +[](ExecutionFact &f) { ++f.epochs.safety_epoch; },
        +[](ExecutionFact &f) { ++f.epochs.calibration_version; },
        +[](ExecutionFact &f) { f.dependency_count = 0; },
        +[](ExecutionFact &f) { f.dependencies[0].source_id = token("other").snapshot(); },
        +[](ExecutionFact &f) { ++f.dependencies[0].source_version; },
        +[](ExecutionFact &f) { ++f.dependencies[0].generation; },
        +[](ExecutionFact &f) { ++f.dependencies[0].sequence; },
        +[](ExecutionFact &f) {
            ayni::foundation::IdAllocator ids;
            ids.start(1);
            EXPECT_TRUE(ids.allocate(f.dependencies[0].subscription_id));
        },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        ASSERT_TRUE(valid_execution_metadata(candidate));
        EXPECT_FALSE(same_execution_identity(original, candidate));
        EXPECT_FALSE(execution_advances(original, candidate));
    }
}

TEST(ExecutionFacts, record_prefixes_never_form_a_complete_execution_tail) {
    for (const auto layout : {ObservationRecordLayout::source, ObservationRecordLayout::scoped}) {
        auto fact = execution_fact();
        if (layout == ObservationRecordLayout::scoped) {
            ayni::foundation::IdAllocator ids;
            ids.start(1);
            ASSERT_TRUE(ids.allocate(fact.dependencies[0].subscription_id));
        }
        std::vector<uint8_t> bytes(execution_record_size(fact));
        ayni::durability::BodyWriter writer(bytes.data(), static_cast<uint32_t>(bytes.size()));
        write_execution(fact, writer, layout);
        ASSERT_TRUE(writer.complete());
        for (uint32_t size = 0; size <= bytes.size(); ++size) {
            ayni::durability::BodyReader reader(bytes.data(), size);
            ExecutionFact decoded{};
            const bool complete = read_execution(reader, decoded, layout) && reader.ok() &&
                                  valid_execution_metadata(decoded);
            EXPECT_EQ(complete, size == bytes.size()) << size;
        }
    }
}
// C-PERMIT-002 / ADR-0050: failed trusted descriptors cannot consume the
// configured slot or replace the original executor ports.
TEST(ExecutionRegistry, invalid_executor_descriptors_leave_capacity_and_original_ports_intact) {
    using ayni::catalog::ExecutionRegistry;
    ayni::providers::mock_execution::Executor executor;
    ayni::providers::mock_execution::Config config;
    config.now_us = [](void *) -> uint64_t { return 0; };
    config.limits.count = 1;
    config.limits.fields[0] = {token("angle").snapshot(), token("degree").snapshot(), -10, 10};
    ASSERT_EQ(executor.configure(config), AYNI_OK);
    ayni_executor_v1 original{};
    ASSERT_EQ(executor.describe_executor(original), AYNI_OK);
    const auto changes = {
        +[](ayni_executor_v1 &p) { p.struct_size = 0; },
        +[](ayni_executor_v1 &p) { p.executor_id = {}; },
        +[](ayni_executor_v1 &p) { p.version = 0; },
        +[](ayni_executor_v1 &p) { p.control_domain_id = 0; },
        +[](ayni_executor_v1 &p) { p.executor_boot_epoch = 0; },
        +[](ayni_executor_v1 &p) { p.safety_epoch = 0; },
        +[](ayni_executor_v1 &p) { p.calibration_version = 0; },
        +[](ayni_executor_v1 &p) { p.control_mode = 0; },
        +[](ayni_executor_v1 &p) { p.procedure_max_us = 0; },
        +[](ayni_executor_v1 &p) { p.session_lease_max_us = 0; },
        +[](ayni_executor_v1 &p) { p.command_age_max_us = 0; },
        +[](ayni_executor_v1 &p) { p.poll_period_us = 0; },
        +[](ayni_executor_v1 &p) { p.poll_period_us = p.procedure_max_us + 1; },
        +[](ayni_executor_v1 &p) { p.session_lease_max_us = p.poll_period_us - 1; },
        +[](ayni_executor_v1 &p) { p.challenge = nullptr; },
        +[](ayni_executor_v1 &p) { p.renew = nullptr; },
        +[](ayni_executor_v1 &p) { p.stop = nullptr; },
        +[](ayni_executor_v1 &p) { p.status = nullptr; },
    };
    std::vector<uint64_t> arena(ExecutionRegistry::storage_size(1, 1) / sizeof(uint64_t) + 1);
    ExecutionRegistry registry(arena.data(), 1, 1);
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        EXPECT_EQ(registry.add_executor(candidate), AYNI_INVALID_ARGUMENT);
        uint32_t index = 99;
        EXPECT_FALSE(registry.find_executor(original.executor_id, original.version, index));
        EXPECT_EQ(index, 99U);
    }
    ASSERT_EQ(registry.add_executor(original), AYNI_OK);
    uint32_t index = 99;
    ASSERT_TRUE(registry.find_executor(original.executor_id, original.version, index));
    EXPECT_EQ(registry.executor(index).ports.ctx, original.ctx);
    EXPECT_EQ(registry.executor(index).ports.challenge, original.challenge);
    EXPECT_FALSE(registry.find_executor(original.executor_id, original.version + 1, index));
    EXPECT_FALSE(registry.find_executor({"absent", 6}, original.version, index));
    EXPECT_EQ(registry.add_executor(original), AYNI_INVALID_ARGUMENT);
    auto second = original;
    second.executor_id = {"other", 5};
    EXPECT_EQ(registry.add_executor(second), AYNI_INVALID_ARGUMENT); // Same exclusive domain.
    ++second.control_domain_id;
    EXPECT_EQ(registry.add_executor(second), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(executor.close(), AYNI_OK);
}

// White-box, ADR-0050: exact selection includes every original version,
// limit and parameter projection; a new catalog cannot amend an old permit.
TEST(ExecutionRegistry, binding_equality_includes_all_versions_limits_and_parameter_projection) {
    const auto original = execution_fact().binding;
    const auto changes = {
        +[](ayni::catalog::ExecutionBinding &b) { b.binding = token("other"); },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.binding_version; },
        +[](ayni::catalog::ExecutionBinding &b) { b.executor = token("other"); },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.executor_version; },
        +[](ayni::catalog::ExecutionBinding &b) { b.scene = token("other"); },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.scene_version; },
        +[](ayni::catalog::ExecutionBinding &b) { b.form = AYNI_FORM_SESSION; },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.lease_us; },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.max_command_age_us; },
        +[](ayni::catalog::ExecutionBinding &b) {
            b.fields[1] = {token("other"), token("degree"), 1, 2};
            b.field_count = 2;
        },
        +[](ayni::catalog::ExecutionBinding &b) { b.fields[0].id = token("other"); },
        +[](ayni::catalog::ExecutionBinding &b) { b.fields[0].unit = token("other"); },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.fields[0].minimum_field; },
        +[](ayni::catalog::ExecutionBinding &b) { ++b.fields[0].maximum_field; },
    };
    ASSERT_EQ(ayni::catalog::validate_execution_binding(original), AYNI_OK);
    EXPECT_TRUE(ayni::catalog::same_execution_binding(original, original));
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        ASSERT_EQ(ayni::catalog::validate_execution_binding(candidate), AYNI_OK);
        EXPECT_FALSE(ayni::catalog::same_execution_binding(original, candidate));
        EXPECT_FALSE(ayni::catalog::same_execution_binding(candidate, original));
    }
}
} // namespace
