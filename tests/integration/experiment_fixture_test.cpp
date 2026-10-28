// Full-fixture infrastructure (ADR-0075): real fault/restart and external count
// self-checks use the same compiled scripts as the simulator CLI.
#include "experiment_support.h"
#include <algorithm>
#include <ayni/runtime/sim_node/experiments.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <vector>
namespace {
using namespace ayni::runtime::experiments;
std::vector<const Case *> parameters() {
    const auto all = cases();
    std::vector<const Case *> out;
    for (std::size_t at = 0; at < all.size; ++at) {
        out.push_back(&all.data[at]);
    }
    return out;
}
class ExperimentFixtures : public testing::TestWithParam<const Case *> {};
TEST_P(ExperimentFixtures, compiled_scenario_records_actual_fault_and_assertion_results) {
    const auto &data = *GetParam();
    auto pattern = (std::filesystem::temp_directory_path() / "ayni-experiment-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    ASSERT_NE(::mkdtemp(buffer.data()), nullptr);
    const auto root = std::filesystem::path(buffer.data());
    const auto result = run(data, (root / "run").c_str());
    EXPECT_EQ(result.status, AYNI_OK) << result.detail << "; artifacts: " << root;
    EXPECT_EQ(result.close_status, AYNI_OK);
    EXPECT_EQ(result.artifact_status, AYNI_OK);
    // Keep failing artifacts for diagnosis; successful runs have replay coverage too.
    if (result.status == AYNI_OK && result.close_status == AYNI_OK &&
        result.artifact_status == AYNI_OK) {
        std::filesystem::remove_all(root);
    }
}
INSTANTIATE_TEST_SUITE_P(Compiled, ExperimentFixtures, testing::ValuesIn(parameters()),
                         [](const testing::TestParamInfo<const Case *> &entry) {
                             std::string name(entry.param->id);
                             std::replace(name.begin(), name.end(), '.', '_');
                             return name;
                         });
// ADR-0075: a failed assertion stays failed even after subsequent matching checks.
TEST(ExperimentEvidence, first_failure_survives_later_success_and_retains_classified_status) {
    Evidence evidence;
    EXPECT_FALSE(evidence.status("allocation", AYNI_CAPACITY_EXCEEDED));
    EXPECT_TRUE(evidence.equal("later", 1, 1));
    EXPECT_FALSE(evidence.ok());
    EXPECT_EQ(evidence.error_status(), AYNI_CAPACITY_EXCEEDED);
    EXPECT_NE(evidence.failure().find("allocation"), std::string::npos);
}
// ADR-0075: arbitrary callback pointers cannot bypass the compiled closed profile.
TEST(ExperimentEvidence, unregistered_case_is_rejected_before_creating_output) {
    const Case fabricated{"HARNESS.S1.fake", "HARNESS S1", "", "", nullptr};
    EXPECT_EQ(run(fabricated, "/unregistered-must-not-be-created").status, AYNI_INVALID_ARGUMENT);
}
} // namespace
