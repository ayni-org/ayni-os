// Native Host for Linux and macOS builds: the platform services the Kernel
// uses to maintain its own invariants (ADR-0006). M0 provides the clock only.

#ifndef AYNI_HOSTS_NATIVE_H
#define AYNI_HOSTS_NATIVE_H

#include <ayni/ayni.h>

namespace ayni::hosts::native {

// Returns a service table that needs no context (ctx is null) and stays valid
// for the whole process, so callers may copy it freely. The clock is
// std::chrono::steady_clock, which never decreases.
ayni_host_v1 host_table();

} // namespace ayni::hosts::native

#endif // AYNI_HOSTS_NATIVE_H
