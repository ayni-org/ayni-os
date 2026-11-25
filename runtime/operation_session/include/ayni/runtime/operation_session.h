// Independently authorized long-running operation, built only from existing public Core APIs.
#ifndef AYNI_RUNTIME_OPERATION_SESSION_H
#define AYNI_RUNTIME_OPERATION_SESSION_H
#include <array>
#include <ayni/calls.h>

namespace ayni::runtime {
// Identity only, never an authorization token. Goal identity is never retargeted or reused.
struct OperationSessionHandle {
    uint64_t boot = 0, owner = 0, goal = 0, generation = 0; // All positive after ready_to_activate.
};
// One bounded operation route; create/cancel use the composition's exclusive command stream.
class OperationSession {
  public:
    // Registered callback address and all Config borrows remain stable until Kernel destroy.
    OperationSession() = default;
    OperationSession(const OperationSession &) = delete;
    OperationSession &operator=(const OperationSession &) = delete;
    OperationSession(OperationSession &&) = delete;
    OperationSession &operator=(OperationSession &&) = delete;
    static constexpr uint32_t input_capacity = 1024, node_capacity = 64;
    struct SubmitResult {
        ayni_status_t status = AYNI_OK; // BUSY means retry same operation through command owner.
        ayni_reason_t reason = AYNI_REASON_NONE; // Classified refusal, no Provider text.
    };
    struct Commands {
        void *context = nullptr; // Outlives Kernel and this object.
        // OK create writes the exact admitted nonzero Goal ID; BUSY retains nothing.
        // Serializes revisions/command IDs with every other composition command.
        SubmitResult (*submit)(void *, ayni_intent_kind_t, const void *, uint64_t *,
                               uint32_t) = nullptr;
    };
    struct Config {
        ayni_string_v1 goal_type{}, strategy_id{}, binding_id{}; // Frozen borrowed catalog IDs.
        uint32_t version = 1, binding_version = 1; // Exact identities; no latest fallback.
        void *policy_context = nullptr; // Only the requirement exporter interprets business scope.
        ayni_goal_requirements_fn requirements = nullptr; // Exact independent Goal needs.
        const ayni_workspace_v1 *workspace = nullptr;     // Exclusive during owner callbacks.
        uint64_t max_lifetime_us =
            0; // Positive local maximum, microseconds; every open is bounded.
        Commands commands;
    };
    enum class Phase { idle, creating, authorizing, ready_to_activate, active, closing, terminal };
    struct Snapshot {
        Phase phase = Phase::idle; // Single lifecycle owner; no implicit boolean state machine.
        OperationSessionHandle handle;
        uint64_t expires_at_us = 0; // Original Goal deadline on Kernel Host clock; never extended.
        uint64_t task = 0;          // Original operation Task, zero until Core creates it.
        ayni_status_t status = AYNI_OK; // Terminal/refusal status; UNKNOWN is never retried.
        ayni_reason_t reason = AYNI_REASON_NONE;
    };
    // Owner before start: registers a parameter-driven one-Call strategy and CANCEL_ON_REBOOT type.
    ayni_status_t install(ayni_kernel_t *, const Config &);
    // Owner outside Core callbacks: copies an exact admitted input; no BODY references allowed.
    // Opening never inherits an old Task/Goal Grant; the new Goal obtains independent authority.
    ayni_status_t open(uint64_t boot, uint64_t owner, const ayni_value_v1 &, uint64_t lifetime_us);
    // Owner each tick after command receipts, now_us on the Kernel Host monotonic clock; no wait or
    // I/O. Missing independent Grant is refused.
    ayni_status_t advance(uint64_t durable_seq, uint64_t now_us);
    // Only after the opener's handle reply was successfully posted; opens the execution gate.
    ayni_status_t activate(OperationSessionHandle, uint64_t now_us);
    // Asynchronous cancellation; keep advancing until terminal. Closing is valid during opening.
    ayni_status_t close(OperationSessionHandle);
    // Shutdown/orphaned opener cleanup, including a creation without a published handle.
    void close_all();
    // Only a terminal record may be released; does not retry UNKNOWN or reopen anything.
    ayni_status_t release();
    Snapshot snapshot() const { return state_; }
    bool owns(ayni_string_v1 type, uint32_t version) const;

  private:
    static ayni_status_t validate(void *, ayni_bytes_v1);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static uint32_t ready(void *, uint64_t);
    ayni_status_t read(ayni_bytes_v1, ayni_value_v1 &);
    bool matches(OperationSessionHandle) const;
    void fail(ayni_status_t, ayni_reason_t);
    ayni_status_t observe(uint64_t durable_seq, uint64_t now_us);
    enum class Installation { empty, failed, ready };
    Installation installation_ =
        Installation::empty;          // A partial install cannot be retried/mutated.
    ayni_kernel_t *kernel_ = nullptr; // Borrowed from successful install through destroy.
    Config config_{};                 // Immutable after install; contains no copied Core state.
    Snapshot state_{};                // The component owns only creation/activation/close handoff.
    std::array<uint8_t, input_capacity>
        input_{};             // Copied open input until Goal creation completes.
    uint32_t input_size_ = 0; // Active input prefix, zero while idle.
    std::array<ayni_value_node_v1, node_capacity> nodes_{}; // Owner-only decode scratch.
    uint64_t generation_ = 0,
             lifetime_us_ = 0; // Never wrap generation; lifetime bound of this open.
};
} // namespace ayni::runtime
#endif
