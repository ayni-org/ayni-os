#include "experiment_support.h"
#include "fixture_json.h"
#include <ayni/runtime/sim_node/experiments.h>
#include <ayni/storage_format.h>
#include <filesystem>
#include <fstream>
#include <new>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t storage_schema = AYNI_STORAGE_SCHEMA_CURRENT; // Actual compiled codec version.
const char *deployment_name(Deployment deployment) {
    switch (deployment) {
    case Deployment::counter:
        return "counter";
    case Deployment::physical:
        return "physical";
    case Deployment::shared:
        return "shared";
    case Deployment::shared_physical:
        return "shared_physical";
    case Deployment::replacement:
        return "replacement";
    case Deployment::composite:
        return "composite";
    }
    return "unsupported";
}
void capture_failure(Result &result, Evidence &evidence) {
    if (result.status == AYNI_OK) {
        result.status = evidence.error_status();
    }
    if (result.detail.empty()) {
        result.detail = evidence.take_failure(); // Moving preserves the cause without allocation.
    }
}
bool write_artifacts(const Case &data, const std::filesystem::path &directory,
                     const Evidence &evidence, const ayni_config_v1 &config, const Result &result) {
    std::size_t kernel_bytes = 0, kernel_alignment = 0;
    if (ayni_kernel_memory_requirements(&config, &kernel_bytes, &kernel_alignment) != AYNI_OK) {
        return false;
    }
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
             << ",\"deployment\":" << fixtures::quoted(deployment_name(data.deployment))
             << ",\"query_authority\":" << data.query_authority
             << ",\"revision\":" << fixtures::quoted(source_revision())
             << ",\"compiler\":" << fixtures::quoted(__VERSION__)
             << ",\"host\":\"native-sim\",\"seed\":\"0\",\"build_type\":"
             << fixtures::quoted(AYNI_FIXTURE_BUILD_TYPE)
             << ",\"sanitizer\":" << fixtures::quoted(AYNI_FIXTURE_SANITIZER)
             << ",\"abi_major\":" << AYNI_ABI_MAJOR << ",\"abi_minor\":" << AYNI_ABI_MINOR
             << ",\"storage_schema\":" << storage_schema
             << ",\"kernel_config\":" << fixtures::configuration_json(config)
             << ",\"kernel_memory_bytes\":" << kernel_bytes
             << ",\"kernel_memory_alignment\":" << kernel_alignment
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
    Evidence evidence;
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
        World world(evidence, data.query_authority, data.deployment);
        result.close_status = AYNI_RESULT_UNKNOWN; // Only explicit close can confirm cleanup.
        if (world.start()) {
            data.execute(world);
        }
        result.close_status = world.close();
        capture_failure(result, evidence);
        if (!write_artifacts(data, directory, evidence, world.config, result)) {
            result.artifact_status = AYNI_INTERNAL;
        }
    } catch (const std::bad_alloc &) {
        capture_failure(result, evidence);
        if (result.status == AYNI_OK) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.detail = "allocation";
        }
        result.artifact_status = AYNI_CAPACITY_EXCEEDED;
    } catch (const std::filesystem::filesystem_error &) {
        capture_failure(result, evidence);
        result.artifact_status = AYNI_INTERNAL;
    }
    return result;
}
} // namespace ayni::runtime::experiments
