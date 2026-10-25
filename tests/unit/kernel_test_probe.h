// White-box access to the parts of a kernel, for the tests that check state
// the C ABI does not show (ADR-0051). Tests reach ayni_kernel's members only
// through this class, so a change to the kernel's representation changes
// this file instead of every test; when ayni_kernel's members become private
// it names this class a friend, as EventIngress does EventIngressTestPeer.
// Behavior is still driven through the C ABI.

#ifndef AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H
#define AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H

#include "kernel/kernel_state.h"

#include <ayni/ayni.h>

namespace ayni::kernel {

class KernelTestProbe {
  public:
    // The kernel's copy of the Host table.
    static ayni_host_v1 &host(ayni_kernel_t *kernel) { return kernel->host; }
    static WorkModel &work(ayni_kernel_t *kernel) { return kernel->work; }
    static WorkReplay &replay(ayni_kernel_t *kernel) { return kernel->replay; }
    static BootSequence &boot(ayni_kernel_t *kernel) { return kernel->boot; }
    static CommitLog &log(ayni_kernel_t *kernel) { return kernel->log; }
    static WriteSchedule &writes(ayni_kernel_t *kernel) { return kernel->writes; }
    static StorageMaintenance &maintenance(ayni_kernel_t *kernel) { return kernel->maintenance; }
    static StorageBudget &budget(ayni_kernel_t *kernel) { return kernel->budget; }
    static EventIngress &ingress(ayni_kernel_t *kernel) { return kernel->ingress; }
    static CompletionPool &completions(ayni_kernel_t *kernel) { return kernel->completions; }
};

} // namespace ayni::kernel

#endif // AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H
