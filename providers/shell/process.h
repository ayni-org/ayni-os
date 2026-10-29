// OS-specific execution boundary. Provider owns scheduling and reliable posting;
// this worker-only function owns the child/process group and all pipe handles.
#ifndef AYNI_PROVIDERS_SHELL_PROCESS_H
#define AYNI_PROVIDERS_SHELL_PROCESS_H
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ayni::providers::shell {
// Structured business facts; both output strings are lossless base64 at posting.
struct ProcessResult {
    enum class End { exited, cancelled, timeout, output_limit, start_failed, io_failed };
    End end = End::start_failed;
    int exit_code = -1;   // Valid iff exited and signal==0.
    int signal = 0;       // Positive terminating POSIX signal, zero for normal exit.
    bool started = false; // exec may have begun; false proves no command execution.
    std::string standard_output;
    std::string standard_error;
};
// Configuration-time prepared process inputs. Environment is copied before the
// worker exists; the child only calls async-signal-safe functions before exec.
struct ProcessConfig {
    std::string directory;
    std::vector<std::string> environment;
    std::vector<char *> environment_view;
    uint32_t timeout_ms = 0;
    uint32_t output_bytes = 0;
};
// Pure observation of Host reaping disposition; never installs a signal handler.
bool reaping_supported();
// Worker-only. Host must keep SIGCHLD waitable and must not separately reap
// these children. Owns normal/exceptional reap and pipe cleanup; lost reap
// ownership returns UNKNOWN without signalling a possibly reused PID. Group
// termination is not a sandbox for descendants deliberately escaping the group.
ProcessResult execute(const ProcessConfig &config, const char *script,
                      const std::atomic<bool> &cancelled);
// Encodes one raw stream losslessly; the structured result labels this base64.
std::string encode_output(const std::string &bytes);
} // namespace ayni::providers::shell
#endif
