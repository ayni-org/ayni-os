// E1 S10 / C-HOST-006: exercise the real finite completion owner. This test
// probe is paired with the public-Kernel saturated-ingress scenario.
#include "experiment_support.h"
#include "kernel/ports/completion_pool.h"
namespace ayni::runtime::experiments {
bool verify_completion_slots(Evidence &e) {
    constexpr uint32_t slots = 2;
    hosts::sim::Host host;
    const auto ports = host.table();
    kernel::AttemptSequence attempts;
    std::vector<uint64_t> arena(kernel::CompletionPool::storage_size(slots) / sizeof(uint64_t) + 1);
    kernel::CompletionPool pool(arena.data(), slots, ports, attempts);
    struct Borrow {
        ayni_attempt_ref_v1 attempt;
        ayni_completion_t *completion;
    };
    struct Context {
        std::vector<Borrow> borrows;
        uint32_t applied = 0;
    } context;
    const auto issue = [&] {
        return pool.issue(
            1,
            [](void *p, const ayni_completion_result_v1 &) {
                ++static_cast<Context *>(p)->applied;
            },
            &context,
            [](void *p, ayni_attempt_ref_v1 attempt, ayni_completion_t *completion) {
                static_cast<Context *>(p)->borrows.push_back({attempt, completion});
                return AYNI_SUBMIT_ACCEPTED;
            },
            &context);
    };
    for (uint32_t at = 0; at < slots; ++at) {
        if (!e.equal("reserve completion before port", static_cast<uint32_t>(issue()),
                     static_cast<uint32_t>(kernel::IssueResult::in_flight))) {
            return false;
        }
    }
    if (!e.equal("exhausted completions return BUSY", static_cast<uint32_t>(issue()),
                 static_cast<uint32_t>(kernel::IssueResult::busy)) ||
        !e.equal("BUSY never calls port", context.borrows.size(), slots)) {
        return false;
    }
    for (const auto &borrow : context.borrows) {
        auto result = initialized<ayni_completion_result_v1>();
        result.outcome = 1;
        result.attempt = borrow.attempt;
        if (!e.status("reserved completion accepted",
                      ayni_completion_post(borrow.completion, &result)) ||
            !e.status("duplicate completion refused",
                      ayni_completion_post(borrow.completion, &result), AYNI_INVALID_STATE)) {
            return false;
        }
    }
    return e.equal("each reserved completion applied once", pool.apply_posted(slots), slots) &&
           e.equal("no duplicate application", pool.apply_posted(slots), 0) &&
           e.equal("all completion slots returned", pool.in_flight(), 0) &&
           e.equal("one callback per accepted request", context.applied, slots);
}
} // namespace ayni::runtime::experiments
