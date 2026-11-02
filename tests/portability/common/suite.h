// Identical normal cases for both independent consumers, public SDK APIs only.
#ifndef AYNI_PORTABILITY_SUITE_H
#define AYNI_PORTABILITY_SUITE_H
#include <ayni/runtime/portable.h>
namespace ayni::portability {
struct Evidence;
struct Clock {
    void *context = nullptr;
    uint64_t (*now_us)(void *) = nullptr;
    void (*yield)(void *) = nullptr;
    Evidence *observe = nullptr; // Optional per-run public-API resource sampling; never global.
};
// Test application state stays alive through Kernel destroy, without private fixtures.
struct Policies {
    strategies::Rules execution;
    strategies::RuleStep turn_step;
    strategies::Rule turn_rule;
    std::array<ayni_value_node_v1, 2> turn_input{};
};
ayni_status_t configure(runtime::portable::Profile &, Policies &);
struct TextExpectation {
    uint32_t bytes = 0;
    uint64_t fingerprint = 0; // In-memory comparison only, not an integrity/security assertion.
};
struct BodyExpectation {
    TextExpectation summary, history;
    static constexpr uint32_t rounds_max = 32; // Retained rounds the normal suite compares.
    std::array<TextExpectation, rounds_max> users{}, answers{};
    uint64_t generation = 0, revision = 0, context_version = 0;
    uint64_t summary_first = 0, summary_last = 0;
};
struct Evidence {
    BodyExpectation body[runtime::portable::agent_count]{};
    uint64_t agents[runtime::portable::agent_count]{};
    uint64_t goals[runtime::portable::agent_count]{};
    uint64_t calls[runtime::portable::agent_count]{};
    uint64_t contexts[runtime::portable::agent_count]{};
    uint32_t rounds[runtime::portable::agent_count]{};
    uint64_t last_round[runtime::portable::agent_count]{};
    uint32_t peak_live_tasks = 0;
    bool model_resource_seen[runtime::portable::agent_count]{};
};
ayni_status_t start(runtime::portable::Profile &, Clock);
ayni_status_t normal(runtime::portable::Profile &, Clock, Evidence &);
ayni_status_t reopened(runtime::portable::Profile &, Clock, const Evidence &);
ayni_status_t stop(runtime::portable::Profile &, Clock);
} // namespace ayni::portability
#endif
