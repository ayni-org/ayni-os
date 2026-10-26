// White-box access to the parts of a kernel, for the tests that check state
// the C ABI does not show (ADR-0051). Tests reach ayni_kernel's members only
// through this class, so a change to the kernel's representation changes
// this file instead of every test. The private Kernel names this test-only
// class a friend, as EventIngress does EventIngressTestPeer (ADR-0058).
// Behavior is still driven through the C ABI.

#ifndef AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H
#define AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H

#include "kernel/runtime/kernel_state.h"

#include <ayni/ayni.h>

#include <cstdint>
#include <ostream>
#include <utility>

namespace ayni::kernel {

class KernelTestProbe {
  public:
    // The kernel's copy of the Host table.
    static ayni_host_v1 &host(ayni_kernel_t *kernel) { return kernel->host; }
    static WorkModel &work(ayni_kernel_t *kernel) { return kernel->work; }
    static const WorkModel &work(const ayni_kernel_t *kernel) { return kernel->work; }
    static WorkReplay &replay(ayni_kernel_t *kernel) { return kernel->replay; }
    static BootSequence &boot(ayni_kernel_t *kernel) { return kernel->boot; }
    static CommitLog &log(ayni_kernel_t *kernel) { return kernel->log; }
    static const CommitLog &log(const ayni_kernel_t *kernel) { return kernel->log; }
    static WriteSchedule &writes(ayni_kernel_t *kernel) { return kernel->writes; }
    static StorageMaintenance &maintenance(ayni_kernel_t *kernel) { return kernel->maintenance; }
    static StorageBudget &budget(ayni_kernel_t *kernel) { return kernel->budget; }
    static bool storage_has_room(const ayni_kernel_t *kernel) { return kernel->storage_has_room(); }
    static EventIngress &ingress(ayni_kernel_t *kernel) { return kernel->ingress; }
    static CompletionPool &completions(ayni_kernel_t *kernel) { return kernel->completions; }
};

// What refresh_due_work answers: whether due work is ready, else when it next
// comes (work::no_time: not without a change).
struct DueAnswer {
    bool due = false;
    uint64_t next_us = 0;
    bool operator==(const DueAnswer &other) const {
        return due == other.due && next_us == other.next_us;
    }
};

// So that a failed comparison shows both answers.
inline void PrintTo(const DueAnswer &answer, std::ostream *out) {
    *out << (answer.due ? "due" : "not due") << ", next " << answer.next_us;
}

// White-box, ADR-0051 decision 11: the answer of the due-work search the
// kernel kept, then of a new search, both at now_us and the log's durable
// prefix. The new search stays kept, as after a step's own search.
inline std::pair<DueAnswer, DueAnswer> kept_and_new_due_search(ayni_kernel_t *kernel,
                                                               uint64_t now_us) {
    auto &model = KernelTestProbe::work(kernel);
    const uint64_t durable_seq = KernelTestProbe::log(kernel).durable_seq();
    DueAnswer kept{};
    kept.due = refresh_due_work(model, now_us, kept.next_us, durable_seq);
    model.forget_due_work();
    DueAnswer fresh{};
    fresh.due = refresh_due_work(model, now_us, fresh.next_us, durable_seq);
    return {kept, fresh};
}

} // namespace ayni::kernel

#endif // AYNI_TESTS_UNIT_KERNEL_TEST_PROBE_H
