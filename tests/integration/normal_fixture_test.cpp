// ADR-0068: the same framework-independent executor drives CLI and GoogleTest.
#include <algorithm>
#include <ayni/runtime/sim_node/fixtures.h>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>
namespace {
using namespace ayni::runtime::fixtures;
std::vector<const Case *> parameters() {
    const auto table = cases();
    std::vector<const Case *> out;
    for (size_t at = 0; at < table.size; ++at) {
        out.push_back(&table.data[at]);
    }
    return out;
}
class NormalFixtures : public testing::TestWithParam<const Case *> {};
TEST_P(NormalFixtures, compiled_normal_scenario_records_successful_execution) {
    const auto &data = *GetParam();
    const std::filesystem::path root(AYNI_NORMAL_ARTIFACT_ROOT);
    std::filesystem::create_directories(root);
    const auto pattern = (root / (std::string(data.id) + "-XXXXXX")).string();
    std::vector<char> directory(pattern.begin(), pattern.end());
    directory.push_back('\0');
    ASSERT_NE(::mkdtemp(directory.data()), nullptr);
    const auto output = std::filesystem::path(directory.data()) / "run";
    const auto result = run(data, output.c_str());
    EXPECT_EQ(result.status, AYNI_OK) << result.detail << "; artifacts: " << output;
    EXPECT_TRUE(std::filesystem::exists(output / "run.json"));
    EXPECT_TRUE(std::filesystem::exists(output / "trace.jsonl"));
}
INSTANTIATE_TEST_SUITE_P(Compiled, NormalFixtures, testing::ValuesIn(parameters()),
                         [](const testing::TestParamInfo<const Case *> &entry) {
                             std::string name = entry.param->id;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
} // namespace
