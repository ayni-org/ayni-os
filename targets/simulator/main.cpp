// ayni-sim: native executable over runtime/ (ADR-0010). It assembles a Kernel
// whose log lives in a new temporary state directory, recovers it (a first
// boot, so it only persists boot epoch 1), reports its lifecycle, shuts it
// down and removes the directory. Experiment runners (--experiment E1..E4)
// arrive with the M1 fixtures.

#include <ayni/ayni.h>
#include <ayni/runtime/native_node.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Recovery and shutdown wait only on the local file; past this something is
// wrong.
constexpr auto step_deadline = 10s;

const char *lifecycle_name(ayni_lifecycle_t state) {
    switch (state) {
    case AYNI_LIFECYCLE_CONFIGURING:
        return "CONFIGURING";
    case AYNI_LIFECYCLE_RECOVERING:
        return "RECOVERING";
    case AYNI_LIFECYCLE_RUNNING:
        return "RUNNING";
    case AYNI_LIFECYCLE_RESTRICTED:
        return "RESTRICTED";
    case AYNI_LIFECYCLE_FENCED:
        return "FENCED";
    case AYNI_LIFECYCLE_DRAINING:
        return "DRAINING";
    case AYNI_LIFECYCLE_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

int fail(const char *step, ayni_status_t status) {
    std::fprintf(stderr, "ayni-sim: %s failed with status %" PRId32 "\n", step, status);
    return 1;
}

// A new directory under the system's temporary directory, removed with its
// contents when the object goes.
class StateDirectory {
  public:
    StateDirectory() {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const std::string pattern = (base / "ayni-sim-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        if (::mkdtemp(name.data()) != nullptr) {
            path_ = name.data();
        }
    }

    ~StateDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    StateDirectory(const StateDirectory &) = delete;
    StateDirectory &operator=(const StateDirectory &) = delete;

    // Empty when the directory could not be created.
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

// Steps the Kernel while it is in state, waiting for the Host's wake whenever
// nothing is ready, and writes the state it leaves to. False, having said
// why, when a call fails or the state outlasts step_deadline.
bool step_while(ayni::runtime::NativeNode &node, ayni_lifecycle_t state, const char *what,
                ayni_lifecycle_t &left_to) {
    const auto deadline = std::chrono::steady_clock::now() + step_deadline;
    for (;;) {
        ayni_status_t status = ayni_get_lifecycle(node.kernel(), &left_to);
        if (status != AYNI_OK) {
            static_cast<void>(fail("ayni_get_lifecycle", status));
            return false;
        }
        if (left_to != state) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "ayni-sim: %s did not end within %lld s\n", what,
                         static_cast<long long>(step_deadline.count()));
            return false;
        }
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        status = ayni_step(node.kernel(), 64, 10000, &report);
        if (status != AYNI_OK) {
            static_cast<void>(fail("ayni_step", status));
            return false;
        }
        if (report.more_ready == 0U) {
            static_cast<void>(node.wait_for_wake(100ms));
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        std::fprintf(stderr, "ayni-sim: unsupported argument '%s'; it takes no options yet\n",
                     argv[1]);
        return 2;
    }

    uint32_t abi_major = 0;
    uint32_t abi_minor = 0;
    ayni_status_t status = ayni_get_abi_version(&abi_major, &abi_minor);
    if (status != AYNI_OK) {
        return fail("ayni_get_abi_version", status);
    }
    std::printf("ayni-sim: ABI %" PRIu32 ".%" PRIu32 " (experimental)\n", abi_major, abi_minor);

    // Declared before the node, so that it goes after the log file is closed.
    const StateDirectory state;
    if (state.path().empty()) {
        std::fprintf(stderr, "ayni-sim: could not create a temporary state directory\n");
        return 1;
    }

    ayni::runtime::NativeNode node;
    status = node.assemble(state.path().c_str());
    if (status != AYNI_OK) {
        return fail("assemble", status);
    }
    ayni_lifecycle_t lifecycle = 0;
    status = ayni_get_lifecycle(node.kernel(), &lifecycle);
    if (status != AYNI_OK) {
        return fail("ayni_get_lifecycle", status);
    }
    std::printf("ayni-sim: kernel created, lifecycle %s\n", lifecycle_name(lifecycle));

    status = ayni_start(node.kernel());
    if (status != AYNI_OK) {
        return fail("ayni_start", status);
    }
    if (!step_while(node, AYNI_LIFECYCLE_RECOVERING, "recovery", lifecycle)) {
        return 1;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    status = ayni_get_kernel_info(node.kernel(), &info);
    if (status != AYNI_OK) {
        return fail("ayni_get_kernel_info", status);
    }
    const bool running = lifecycle == AYNI_LIFECYCLE_RUNNING;
    if (running) {
        std::printf("ayni-sim: recovered, lifecycle %s, boot epoch %" PRIu64 "\n",
                    lifecycle_name(lifecycle), info.boot_epoch);
    } else {
        std::printf("ayni-sim: recovery ended in %s, restriction %" PRIu32 "\n",
                    lifecycle_name(lifecycle), info.restriction);
    }

    status = ayni_begin_shutdown(node.kernel());
    if (status != AYNI_OK) {
        return fail("ayni_begin_shutdown", status);
    }
    if (!step_while(node, AYNI_LIFECYCLE_DRAINING, "shutdown", lifecycle)) {
        return 1;
    }
    std::printf("ayni-sim: shut down, lifecycle %s\n", lifecycle_name(lifecycle));

    status = node.release();
    if (status != AYNI_OK) {
        return fail("release", status);
    }
    std::printf("ayni-sim: kernel destroyed\n");
    return running ? 0 : 1;
}
