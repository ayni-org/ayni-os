// Native shell execution for explicitly authorized trusted deployments.
// The working directory is a start location, not a filesystem/process sandbox.
#ifndef AYNI_PROVIDERS_SHELL_H
#define AYNI_PROVIDERS_SHELL_H
#include <ayni/calls.h>
#include <cstdint>
#include <memory>
#include <string>

namespace ayni::providers::shell {
// Immutable startup bounds. The startup environment is copied and never logged.
struct Config {
    std::string working_directory; // Required existing absolute directory; copied at configure.
    uint32_t request_capacity = 1; // Concurrent retained requests, 1..4; one worker executes them.
    uint32_t command_max_bytes = 8192; // UTF-8 shell script, 1..16384 bytes, no embedded NUL.
    uint32_t output_max_bytes = 4096;  // Combined raw stdout/stderr bytes, 1..32768.
    uint32_t timeout_ms = 30000;       // Includes worker execution; 1..300000 milliseconds.
};

// Contract maxima of one configuration; the runtime sizes the Kernel's Call strides from them.
uint32_t call_input_bytes(const Config &config);
uint32_t call_result_bytes(const Config &config);

// Owner-thread configuration/ports, with one I/O worker and bounded copied slots.
// /bin/sh -c supports scripts and pipelines. Every command needs Core authority
// for system.shell; this Provider does not grant access or authenticate a caller.
// Host must keep SIGCHLD waitable and leave these children to this Provider's
// exclusive reaper; another waitpid(-1) service is incompatible without coordination.
class Provider {
  public:
    Provider();
    ~Provider(); // Explicitly close after Kernel destruction; never free a live callback context.
    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    ayni_status_t configure(const Config &config);
    // Offer/context views remain valid through quiesce and Kernel destruction.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    // Explicit unrestricted-shell authority domain; registering it grants nothing.
    ayni_status_t describe_authority(ayni_capability_desc_v1 &out) const;
    // After Core has drained, join worker; keeps registered context alive.
    ayni_status_t quiesce();
    // After Core destruction, release storage; BUSY while a producer/post remains.
    ayni_status_t close();
    uint32_t pending() const;     // Includes queued execution and terminal post still returning.
    ayni_status_t status() const; // First delivery failure, never raw command output.

  private:
    struct Impl; // Owns child processes, copied scripts/output, worker and descriptor.
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::providers::shell
#endif
