// Typed FakeTransport for MA-038 normal fixtures; no wire codec/authentication.
#ifndef AYNI_RUNTIME_MULTINODE_TRANSPORT_H
#define AYNI_RUNTIME_MULTINODE_TRANSPORT_H
#include <array>
#include <ayni/calls.h>
#include <cstdint>
namespace ayni::runtime::multinode {
struct Route {
    ayni_kernel_t *caller;
    ayni_kernel_t *receiver;
    uint64_t caller_instance, service_instance, receiver_management_session;
    uint64_t first_command;        // Exclusive lower bound of the fixture command namespace.
    const char *service_goal_type; // Borrowed through both Kernel lifetimes.
    void *clock_context;
    uint64_t (*now_us)(void *);
    uint64_t invoke_delay_us, query_delay_us;
};
// Node-local IDs always need their node qualifier in artifacts. This simulation
// snapshot is not a durable wire-protocol conclusion.
struct Reply {
    uint64_t operation = 0, caller_task = 0, caller_instance = 0;
    uint64_t service_goal = 0, service_task = 0, service_instance = 0;
    uint64_t acceptance_seq = 0, durable_seq = 0;
    ayni_task_lifecycle_t task_state = 0;
    ayni_call_effect_t effect = 0;
    bool completed = false;
};
class FakeTransport {
  public:
    static constexpr size_t capacity = 4;
    FakeTransport();
    FakeTransport(const FakeTransport &) = delete;
    FakeTransport &operator=(const FakeTransport &) = delete;
    ayni_provider_v1 describe();  // Register metadata before connect; keep this address.
    ayni_status_t connect(Route); // Composition root supplies one trusted peer mapping.
    ayni_status_t advance();      // Owner thread outside either Kernel's callbacks.
    // One typed query in flight, bound to this peer. Absence gives no conclusion
    // about execution, especially while the original invoke is still in transit.
    ayni_status_t query(uint64_t operation);
    ayni_status_t take_reply(Reply &); // Outputs unchanged on error.
    void cancel_query(); // Owner shutdown may forget the read-only query, never an invoke.
    bool pending() const;
    uint64_t received() const { return received_; }
    uint64_t delivered() const { return delivered_; }
    uint64_t last_operation() const;
    uint64_t last_service_goal() const;

  private:
    enum class Phase { empty, transit, accepted, terminal, delivered };
    struct Entry {
        Phase phase = Phase::empty;
        uint64_t call = 0, operation = 0, due_us = 0, acceptance_seq = 0;
        uint64_t caller_task = 0, goal = 0;
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        ayni_provider_result_v1 terminal{};
    };
    enum class QueryPhase { empty, transit, ready };
    struct Query {
        QueryPhase phase = QueryPhase::empty;
        uint64_t operation = 0, due_us = 0;
        ayni_status_t status = AYNI_NOT_READY;
        Reply reply{};
    };
    Route route_{};
    bool connected_ = false;
    uint64_t command_ = 0, received_ = 0, delivered_ = 0;
    std::array<Entry, capacity> entries_{};
    Query query_{};
    ayni_call_resource_v1 resource_{};
    ayni_call_capability_v1 capability_{};
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    ayni_status_t receive(Entry &);
    ayni_status_t observe(const Entry &, Reply &, ayni_provider_result_v1 &) const;
    ayni_status_t poll(Entry &);
};
} // namespace ayni::runtime::multinode
#endif
