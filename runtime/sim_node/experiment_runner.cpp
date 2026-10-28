#include "experiment_support.h"
#include "fixture_json.h"
#include <ayni/runtime/sim_node/experiments.h>
#include <filesystem>
#include <fstream>
#include <new>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t storage_schema = 2; // contracts/storage/records.md, ADR-0038.
bool write_artifacts(const Case &data, const std::filesystem::path &directory,
                     const Evidence &evidence, const ayni_config_v1 &config, const Result &result) {
    std::ofstream trace(directory / "trace.jsonl");
    for (const auto &line : evidence.lines()) {
        trace << line << '\n';
    }
    trace.flush();
    const auto artifact_status = trace.good() ? result.artifact_status : AYNI_INTERNAL;
    std::ofstream manifest(directory / "run.json");
    manifest << "{\"fixture_schema\":1,\"profile\":\"m1-experiment\",\"case_id\":"
             << fixtures::quoted(data.id)
             << ",\"experiment_step\":" << fixtures::quoted(data.experiment_step)
             << ",\"source\":" << fixtures::quoted(data.source)
             << ",\"clauses\":" << fixtures::quoted(data.clauses)
             << ",\"revision\":" << fixtures::quoted(source_revision())
             << ",\"compiler\":" << fixtures::quoted(__VERSION__)
             << ",\"host\":\"native-sim\",\"seed\":\"0\",\"build_type\":"
             << fixtures::quoted(AYNI_FIXTURE_BUILD_TYPE)
             << ",\"sanitizer\":" << fixtures::quoted(AYNI_FIXTURE_SANITIZER)
             << ",\"abi_major\":" << AYNI_ABI_MAJOR << ",\"abi_minor\":" << AYNI_ABI_MINOR
             << ",\"storage_schema\":" << storage_schema
             << ",\"kernel_config\":" << fixtures::configuration_json(config)
             << ",\"status\":" << result.status << ",\"close_status\":" << result.close_status
             << ",\"artifact_status\":" << artifact_status << ",\"passed\":"
             << (result.status == AYNI_OK && result.close_status == AYNI_OK &&
                         artifact_status == AYNI_OK
                     ? "true"
                     : "false")
             << ",\"first_failure\":" << fixtures::quoted(result.detail)
             << ",\"hardware\":\"unverified\"}\n";
    manifest.flush();
    return trace.good() && manifest.good();
}
} // namespace
Result run(const Case &data, const char *output_directory) {
    Result result;
    try {
        const auto all = cases();
        bool registered = false;
        for (std::size_t at = 0; at < all.size; ++at) {
            registered = registered || &data == &all.data[at];
        }
        if (!registered || !output_directory || !*output_directory) {
            return {AYNI_INVALID_ARGUMENT, AYNI_OK, AYNI_OK, "unregistered case or output"};
        }
        std::error_code error;
        const std::filesystem::path directory(output_directory);
        if (!std::filesystem::create_directory(directory, error)) {
            return {AYNI_INVALID_STATE, AYNI_OK, AYNI_INVALID_STATE,
                    "output must be a new directory"};
        }
        Evidence evidence;
        World world(evidence);
        if (world.start()) {
            data.execute(world);
        }
        result.status = evidence.error_status();
        result.detail = evidence.failure();
        result.close_status = world.close();
        // Closing can discover a failure after the script itself succeeded.
        if (result.status == AYNI_OK) {
            result.status = evidence.error_status();
            result.detail = evidence.failure();
        }
        if (!write_artifacts(data, directory, evidence, world.config, result)) {
            result.artifact_status = AYNI_INTERNAL;
        }
    } catch (const std::bad_alloc &) {
        result.status = AYNI_CAPACITY_EXCEEDED;
        result.detail = "fixture allocation";
    } catch (const std::filesystem::filesystem_error &) {
        result.artifact_status = AYNI_INTERNAL;
    }
    return result;
}
} // namespace ayni::runtime::experiments
