// Bounded executor-to-owner answers (ADR-0052). Each registered executor has
// three lanes, so a stop request never waits behind a challenge, a renewal or a
// status read. A lane holds at most one request; any non-ISR thread may post
// its one answer, copied under the Host ingress lock. Only the owner opens,
// reads and closes lanes, and a post for a request a lane no longer holds is
// refused. Like the Provider mailbox, this owns transport, not permits.
#ifndef AYNI_CORE_KERNEL_COMPLETION_EXECUTOR_MAILBOX_H
#define AYNI_CORE_KERNEL_COMPLETION_EXECUTOR_MAILBOX_H

#include <ayni/execution.h>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ayni::kernel {
class ExecutorMailbox;
}

// Immutable target address of one lane; response.request names the request.
struct ayni_executor_completion {
    ayni::kernel::ExecutorMailbox *mailbox; // Borrowed arena owner until producer threads join.
    uint32_t executor;                      // Registered executor slot.
    uint32_t lane;                          // ExecutorLane value.
};

namespace ayni::kernel {

enum class ExecutorLane : uint32_t {
    control, // Challenges for a permit or a renewal, and renewals.
    stop,    // A permit's stop requests.
    status,  // Status reads.
};

// One open request. The owner writes and reads it; a producer is checked only
// against identity and kind, which its port received.
struct ExecutorRequest {
    ayni_attempt_ref_v1 identity; // Exact request identity, never reused.
    ayni_executor_request_t kind; // What was asked.
    uint64_t call_id;             // The call the request serves.
    uint64_t deadline_us;         // Core clock; unanswered by then it is RESULT_UNKNOWN.
    bool after_terminal = false;  // A STATUS read sent after the Provider's terminal was seen.
};

// A copied answer; output is zero unless an OK CHALLENGE or STATUS carried one.
struct ExecutorAnswer {
    ayni_status_t status;
    ayni_executor_output_v1 output;
};

class ExecutorMailbox {
  public:
    static constexpr uint32_t lane_count = 3;
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t executors);
    // Storage is caller-owned for our lifetime, aligned to storage_alignment
    // and storage_size(executors) bytes long; zero executors needs none.
    ExecutorMailbox(void *storage, uint32_t executors, const ayni_host_v1 &host);
    ExecutorMailbox(const ExecutorMailbox &) = delete;
    ExecutorMailbox &operator=(const ExecutorMailbox &) = delete;

    // Owner: holds request on a closed lane and returns the target its port
    // receives; null when the lane already holds one.
    ayni_executor_completion_t *open(uint32_t executor, ExecutorLane lane,
                                     const ExecutorRequest &request);
    // Owner: the lane's request; null when it is closed.
    const ExecutorRequest *request(uint32_t executor, ExecutorLane lane) const;
    // Owner: copies the posted answer; false while none has arrived.
    bool answer(uint32_t executor, ExecutorLane lane, ExecutorAnswer &out) const;
    // Owner: the port refused the lane's request, so that status is its
    // answer, replacing anything the executor posted against its own refusal.
    void refuse(uint32_t executor, ExecutorLane lane, ayni_status_t status);
    // Owner: ends the lane's request and drops its answer; later posts are refused.
    void close(uint32_t executor, ExecutorLane lane);
    // Owner: the earliest deadline of an unanswered request; UINT64_MAX without one.
    uint64_t next_deadline_us() const;
    bool idle() const; // Owner: no lane holds a request.

    // Validates this mailbox's target address, not the request it now holds.
    bool owns(const ayni_executor_completion_t *target) const;
    // Any producer: the one answer of the request the lane holds. INVALID_STATE
    // when it holds another or none, or was answered; malformed answers change nothing.
    ayni_status_t post(const ayni_executor_completion_t &target,
                       const ayni_executor_response_v1 &response);

  private:
    struct Lane {
        ayni_executor_completion target;        // Immutable address across requests.
        std::optional<ExecutorRequest> request; // Empty is closed; owner writes under lock.
        std::optional<ExecutorAnswer> answer;   // Only with a request, at most one; under lock.
    };
    Lane &at(uint32_t executor, ExecutorLane lane);
    const Lane &at(uint32_t executor, ExecutorLane lane) const;

    const ayni_host_v1 &host_; // Borrowed lock/wake ports, valid through producer join.
    Lane *lanes_;              // Caller arena, lane_count lanes per executor.
    uint32_t executors_;       // Immutable registered-executor capacity.
};
} // namespace ayni::kernel
#endif
