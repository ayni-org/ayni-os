// Physical execution control (ADR-0050, ADR-0051, ADR-0052): for the admitted
// physical calls, the Scene and observation facts an admission saves, the
// permit an executor receives after the durable dispatch barrier, its renewal
// and polling, and the measured stop the work model records. The executor
// ports (ayni/execution.h) take requests on the owner thread inside call_out;
// their answers come back through ExecutorMailbox and are applied in a later
// item, never in the one that sent the request, so a local executor that
// answers inside its port takes the same path as a remote one. An urgent stop
// comes before any other owner work and waits for neither the journal nor
// another request of the same executor.
//
// CallRuntime owns the Provider transport that carries a permit and asks
// control at each gate; control sees that transport only through Transport.
// Nothing is copied here: leases belong to resources::PermitTable and
// execution facts to work::CallTable.

#ifndef AYNI_CORE_KERNEL_EXECUTION_EXECUTION_CONTROL_H
#define AYNI_CORE_KERNEL_EXECUTION_EXECUTION_CONTROL_H

#include "catalog/binding/binding_registry.h"
#include "kernel/ports/attempt_sequence.h"
#include "kernel/ports/executor_mailbox.h"
#include "kernel/service/model/work_model.h"
#include "knowledge/observation/observation_table.h"
#include "resources/admission/resource_admission.h"
#include "resources/permit/permit_table.h"

#include <cstdint>
#include <optional>

namespace ayni::kernel {

// How far the owner may take calls and their execution now. Closing discards
// only unpublishable volatile receipts.
enum class RuntimeMode { running, draining, fenced, closing };
// One bounded owner item; nothing waits.
enum class RuntimeAdvance { idle, applied, internal, attempt_exhausted };
// How far ExecutionControl::issue took a call's permit.
enum class PermitIssue {
    issued,    // The lease is installed, or the call needs none.
    requested, // A challenge went out now; the call waits for its answer.
    waiting,   // The challenge is unanswered and its deadline has not passed.
    busy,      // The executor holds another call's permit or challenge.
    refused,   // Refused, malformed or unanswered: no permit was issued.
    exhausted, // No request identity is left.
    defect,    // An owner invariant failed.
};

class ExecutionControl {
  public:
    // What control reads of the Provider transport that carries a permit.
    struct Transport {
        void *context;
        // Whether the runtime still holds call_id, and whether that delivery's
        // deadline has passed.
        bool (*holds)(const void *context, uint64_t call_id, uint64_t now_us, bool &out_expired);
        // The delivery is drained; only the stop proof keeps the call.
        bool (*retiring)(const void *context, uint64_t call_id);
        // The lease is released after its durable stop: a retiring call ends.
        void (*retire)(void *context, uint64_t call_id, uint64_t now_us);
    };

    // All owners outlive control; transport's context is the runtime holding it.
    ExecutionControl(WorkModel &work, resources::ResourceAdmission &resources,
                     resources::PermitTable &permits,
                     const knowledge::ObservationTable &observations, ExecutorMailbox &executors,
                     AttemptSequence &attempts, const ayni_host_v1 &host, Transport transport)
        : work_(work), resources_(resources), permits_(permits), observations_(observations),
          executors_(executors), attempts_(attempts), host_(host), transport_(transport) {}
    ExecutionControl(const ExecutionControl &) = delete;
    ExecutionControl &operator=(const ExecutionControl &) = delete;

    // Admission of a physical contract saves the exact enabled Scene
    // generation and measured prerequisites in out_fact, beside the canonical
    // input; nothing for any other contract. No permit or nonce is issued.
    authority::GrantCheck prepare(const catalog::CallContract &contract,
                                  const catalog::Binding &binding, ayni_bytes_v1 domain,
                                  const work::CallInputView &input, uint64_t now_us,
                                  uint64_t durable_seq, uint64_t task_id,
                                  std::optional<work::ExecutionFact> &out_fact) const;
    // Immediately before dispatch and at every renewal: the saved Scene
    // generation, boot, owner binding and observations still hold.
    bool allowed(uint32_t index, uint64_t now_us, uint64_t durable_seq) const;
    // After the durable DISPATCHING barrier, called again until it settles:
    // the first call sends a challenge, and its answer's fresh nonce anchors
    // the executor's deadline. The caller rechecks authority, resources and
    // allowed before every call, so also after the answer arrived.
    PermitIssue issue(uint32_t index, const resources::ResourceClaim &claim,
                      ayni_bytes_v1 arguments, uint64_t now_us);
    // Read-only: whether issue would do more than wait for an answer.
    bool issue_ready(uint32_t index, uint64_t now_us) const;
    // The call left the dispatch path before its permit: its unanswered
    // challenge is closed, and the answer is refused when it comes.
    void abandon_issue(uint64_t call_id);
    // The permit a Provider request borrows; null for a call without a lease.
    const ayni_execution_permit_v1 *permit(uint64_t call_id) const;
    // Whether the evidence an output depends on is still current.
    work::ResultDependencies result_dependencies(uint32_t index) const;

    // The Provider reported the call's end, so the executor's authoritative
    // absence may later prove it never started; false without a lease.
    bool delivery_ended(uint64_t call_id);
    bool request_stop(uint64_t call_id); // Asks the executor to stop; false without a lease.
    // The executor measured the stop and commit_stop has yet to record it.
    bool stop_measured(uint64_t call_id) const;
    // Records the measured stop in the call's facts at a new stamp; the
    // resource quarantine lifts only after that stamp is durable.
    RuntimeAdvance commit_stop(uint32_t index);
    bool holds_lease(uint64_t call_id) const;
    bool idle() const; // No executor lease or open request remains.
    // Earliest poll or request deadline; UINT64_MAX without either.
    uint64_t next_deadline_us() const;

    // Latch every closed execution gate without I/O. A later resume cannot renew
    // the old permit; budgeted owner items still deliver stop and await proof.
    bool latch_closed_gates(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode);
    // Stops whose gate closed, before journal admission or any other work.
    RuntimeAdvance advance_urgent(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode);
    // One routine poll, renewal or release, scanning leases fairly.
    RuntimeAdvance advance(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode);
    // Read-only: whether advance_urgent or advance has work now.
    bool ready(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode) const;

  private:
    // One unchanged enabled Scene must cover every row of the canonical input.
    ayni_status_t authorize_input(const catalog::ExecutionBinding &binding,
                                  ayni_string_v1 capability, uint32_t version, ayni_bytes_v1 domain,
                                  const catalog::CallContract &contract,
                                  const work::CallInputView &input, uint64_t durable_seq,
                                  uint32_t &scene) const;
    using Advance = RuntimeAdvance;
    using Mode = RuntimeMode;
    // How a lane's request ended when it is taken.
    enum class Reply { pending, answered, expired };

    Advance poll(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode);
    // One taken answer of a lease's lane; allowed is poll's fresh gate.
    Advance apply(resources::PermitLease &lease, ExecutorLane lane, uint64_t now_us, bool allowed);
    // Sends a new request of kind for call_id through port on a closed lane
    // of executor. A refusal stays on the lane as its answer and is read like
    // a posted one.
    template <typename Port>
    Advance send(uint32_t executor, ExecutorLane lane, ayni_executor_request_t kind,
                 uint64_t call_id, bool after_terminal, uint64_t now_us, Port port);
    // The lane's request once answered or past its deadline (then
    // RESULT_UNKNOWN), closing the lane; pending leaves it open.
    Reply take(uint32_t executor, ExecutorLane lane, uint64_t now_us, ExecutorRequest &out_request,
               ExecutorAnswer &out_answer);
    bool due(uint32_t executor, ExecutorLane lane, uint64_t now_us) const;
    bool any_due(uint32_t executor, uint64_t now_us) const;
    bool executor_of(const work::ExecutionFact &fact, uint32_t &out_index) const;
    // Stopping, and its stop request is neither open nor sent.
    bool stop_unsent(const resources::PermitLease &lease) const;
    // One gate for renewal and immediate stopping.
    bool should_stop(const resources::PermitLease &lease, uint64_t now_us, uint64_t durable_seq,
                     Mode mode) const;
    // Durable proof, or explicit unwritable shutdown after drain.
    bool stopped_releasable(const resources::PermitLease &lease, uint64_t durable_seq,
                            Mode mode) const;
    // Shared by dispatch and result: the saved observations are still current.
    bool dependencies_current(uint32_t index, uint64_t now_us) const;

    WorkModel &work_;                         // Call, Scene, executor catalog and owner facts.
    resources::ResourceAdmission &resources_; // Whether a held claim may still execute.
    resources::PermitTable &permits_;         // Resource-owned local leases.
    const knowledge::ObservationTable &observations_; // Dependency validity stays knowledge's.
    ExecutorMailbox &executors_;                      // Request lanes and answers per executor.
    AttemptSequence &attempts_;                       // Request identities, shared with Provider.
    const ayni_host_v1 &host_;                        // Clock for a result's dependencies.
    Transport transport_;
    uint32_t cursor_ = 0; // Fair scan of executor leases.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_EXECUTION_EXECUTION_CONTROL_H
