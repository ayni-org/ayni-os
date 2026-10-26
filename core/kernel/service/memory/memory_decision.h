// Owns one decide lease and the complete accumulated source set, not persistent memory facts.
#ifndef AYNI_CORE_KERNEL_MEMORY_DECISION_H
#define AYNI_CORE_KERNEL_MEMORY_DECISION_H
#include "kernel/service/memory/memory_permissions.h"
namespace ayni::kernel {
class MemoryDecision {
  public:
    // Inherits Goal input and every earlier accepted Task output before exposing a snapshot.
    MemoryDecision(WorkModel &model, uint32_t task_index, uint64_t durable_seq, OwnerClock clock,
                   uint8_t *staging);
    // Borrowed public view valid only while model.calling_out(); close revokes it immediately.
    const ayni_memory_view_v1 *view() const { return &view_; }
    void close() { active_ = false; }
    // Sticky capture errors and fresh source/Grant checks gate the complete decision publication.
    ayni_status_t validate() const;
    // Staged exact export permissions are joined before accepting the proposed call.
    knowledge::MemoryDependencies &dependencies() { return dependencies_; }
    // Validate the exact proposal, complete sources, CAS and capacity before any mutation.
    ayni_status_t check_proposal(const ayni_policy_intent_v1 &intent);
    // Install the preflighted candidate; identity allocation and step share the caller stamp.
    bool accept(foundation::TransitionStamp stamp);
    // Publish sources with the same stamp as a successful checkpoint/call/memory proposal.
    void publish(foundation::TransitionStamp stamp);

  private:
    // One staged candidate; body resides in DecisionBuffer, metadata has no retained ABI pointers.
    struct Proposal {
        uint64_t target_id = 0;            // Zero creates a new identity at acceptance.
        uint32_t expected_revision = 0;    // Exact target CAS, zero iff creating.
        catalog::CatalogId step;           // New Task step owning this write.
        knowledge::MemoryValue value{};    // Unconfirmed content plus final full source union.
        knowledge::MemoryWritePlan plan{}; // Written only by successful final preflight.
    };
    static ayni_status_t search(void *, const ayni_memory_search_v1 *, ayni_memory_purpose_t,
                                ayni_memory_info_v1 *, uint32_t, uint32_t *);
    static ayni_status_t propose(void *, const ayni_memory_proposal_v1 *);
    ayni_status_t stage(const ayni_memory_proposal_v1 &);
    static ayni_status_t read(void *, uint64_t, ayni_memory_purpose_t, ayni_memory_info_v1 *,
                              char *, uint32_t, uint32_t *);
    // Capture the complete finite source closure atomically before any result bytes leave Core.
    ayni_status_t capture(uint32_t index, knowledge::MemoryPurpose purpose);
    // Reuse already captured authority, including an export grant, on a repeated read.
    void retain_export(knowledge::MemoryDependency &dependency) const;
    WorkModel &model_;     // Authoritative single owner; unchanged during the callback.
    uint32_t task_index_;  // Selected retained Task, stable for this decision.
    uint64_t durable_seq_; // Same acknowledged prefix throughout this synchronous lease.
    OwnerClock clock_;     // Fresh monotonic reads check expiry before exposure and publication.
    knowledge::MemoryDependencies dependencies_{}; // Complete staged union, never truncated.
    ayni_status_t failure_ =
        AYNI_OK;         // First capture failure; ignored callback errors cannot hide it.
    bool active_ = true; // Lease ends exactly once when decide returns.
    uint8_t *staging_; // Borrowed private candidate buffer; decision owner scrubs it after return.
    std::optional<Proposal> proposal_; // Empty until a single copied candidate is accepted.
    ayni_memory_view_v1 view_{this, read, search,
                              propose}; // No mutable model pointer is exposed publicly.
};
} // namespace ayni::kernel
#endif
