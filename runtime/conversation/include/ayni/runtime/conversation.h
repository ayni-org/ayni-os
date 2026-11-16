// Portable conversation/file acknowledgement application. No platform selection or I/O loop.
#ifndef AYNI_RUNTIME_CONVERSATION_H
#define AYNI_RUNTIME_CONVERSATION_H
#include <atomic>
#include <ayni/ayni.hpp>
#include <ayni/context.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/body_collection.h>
#include <ayni/strategies/builtin.h>
#include <optional>
#include <string>
namespace ayni::runtime::conversation {
// Composition-owned command identities/session. submit copies body synchronously;
// no callback may reenter Session. The caller observes its own command receipts.
struct Commands {
    void *context = nullptr;
    Submission (*submit)(void *, ayni_intent_kind_t, const void *, uint32_t) = nullptr;
};
// Retain is the existing durable-draft behavior. Discard leaves completed history intact.
enum class UnfinishedTurn {
    retain,            // Preserve the draft; new input requires explicit abandon.
    retain_and_append, // Native conversation policy: new input explicitly appends to the draft.
    discard            // Preserve completed history, discard the terminal draft.
};
// One terminal observation; Core still owns the original Goal/Task and Call evidence.
struct TurnResult {
    uint64_t goal = 0, task = 0;    // Original Core identities; zero if none was admitted.
    ayni_status_t status = AYNI_OK; // Acceptance/terminal observation; NOT_READY for no answer.
    ayni_reason_t reason = AYNI_REASON_NONE; // Original public reason, NONE without detail.
    bool published = false; // True only after the exact publication ACK became durable.
};
struct Config {
    ayni_kernel_t *kernel = nullptr; // Borrowed through shutdown/Host join.
    uint64_t session = 0;            // Bound owner's MANAGEMENT session this boot.
    uint64_t owner_instance = 0;     // Exact Agent owner; context must already be opened/durable.
    ayni_string_v1 goal_type{}; // Borrowed registered ToolLoop GoalType id through destruction.
    const ayni_workspace_v1 *workspace =
        nullptr;                         // Exclusive during each owner call, may be shared.
    hosts::storage::FileValuePort files; // Worker callback context remains alive through Host join.
    BodyCollection bodies; // The ToolLoop body/collection ports, borrowed through Host join.
    Commands commands;
    UnfinishedTurn unfinished = UnfinishedTurn::retain;
    strategies::Persistence persistence = strategies::Persistence::durable;
};
// Caller places this bounded object in suitable RAM (it contains large records).
// Owner-thread, no reentry. poll performs bounded public-API/queue work, never waits.
// After any accepted file request, this object/buffers live until Host close joins.
// Loads the exact owner/version; normal reopen may reattach an original Goal by its
// source baseline or finish an existing publication/erasure. Missing/conflicting evidence
// is refused, never silently initialized, retried or rebound.
class Session {
  public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ayni_status_t configure(Config);
    // Core Task holds are reported without detaching: inspect latest_goal's Task
    // through the public API, resolve with owner authority, then continue polling.
    // Bridge file/version errors are sticky. BUSY is ordinary queue backpressure.
    ayni_status_t poll();
    // OK copies input. Durable mode saves its draft before admission; background mode stages
    // it and starts the Goal while saving proceeds. One turn at a time per Session.
    // Completion is ready() with a new last_round; the final answer crosses Core ACK durability.
    ayni_status_t begin(ayni_string_v1 user, uint64_t memory_id = 0);
    ayni_status_t forget(ayni_context_action_t action, uint64_t turn = 0);
    // Owner-thread, nonblocking. Cancel waits for actual durable Core termination before
    // discarding a draft. Publication already in flight completes and is never rolled back.
    ayni_status_t cancel();
    // Explicitly discard a retained terminal draft; never resumes/retries the old action.
    ayni_status_t abandon();
    // Before Kernel shutdown: discard mode cancels; retaining modes keep the original
    // live Goal for Core shutdown/reopen. Completed publication/ACK and body work drain
    // before OK. BUSY retains all borrows; never closes the Kernel or another Session.
    ayni_status_t quiesce();
    // Delivers once after durable publication/discard, or a retained terminal draft.
    bool take_result(TurnResult &out);
    // Borrow only until the next owner operation. Consume after any required body read succeeds.
    const TurnResult *pending_result() const;
    // Last poll observed a Core Task hold; resolves on that same Task.
    bool held() const { return held_; }
    // A turn owns work/publication or a retained terminal draft; initialization/forget are
    // separate.
    bool active() const;
    // Only the original Core turn can currently make progress. Hosts may consider
    // its proposal/hold idle; file, cleanup and publication phases still need polling.
    bool awaiting_turn() const { return phase_ == Phase::turn; }

    // Rechecks the current Core barrier, including owner changes since the last poll.
    bool ready() const;
    // Borrow only until the next owner operation/Kernel mutation. Each lookup
    // rechecks privacy; retaining a pointer is not native-process isolation.
    const strategies::ConversationRecord *record() const {
        auto anchor = initialized<ayni_context_info_v1>();
        return intake(anchor) == AYNI_OK ? &current_ : nullptr;
    }
    uint64_t latest_goal() const { return goal_; } // Zero until a turn Goal was accepted.
    bool file_pending() const {
        return phase_ == Phase::input || phase_ == Phase::loading || phase_ == Phase::saving;
    }

  private:
    enum class Phase {
        empty,
        load,
        loading,
        ready,
        input,
        save,
        saving,
        start_turn,
        turn,
        terminal,
        prepare_publication, // Completed checkpoint copied once; candidate is not yet confirmed.
        publishing, // Candidate confirmed and PUBLISH accepted; await its durable Core anchor.
        publish,    // Reopened/durable-mode publication still needs its checkpoint candidate.
        acknowledge,
        acknowledged,
        collect,
        collecting,
        erase,
        failed
    };
    enum class Save { initialize, draft, publication, erasure, retained, discard };
    static void complete(void *, const hosts::storage::FileValueResult &) noexcept;
    ayni_status_t context(ayni_context_info_v1 &) const;
    ayni_status_t intake(ayni_context_info_v1 &) const;
    ayni_status_t current_context(ayni_context_info_v1 &) const;
    Submission command(ayni_intent_kind_t, const ayni_context_info_v1 &, uint64_t task = 0,
                       ayni_context_action_t action = AYNI_CONTEXT_IDLE, uint64_t turn = 0);
    bool matches(const strategies::ConversationRecord &, const ayni_context_info_v1 &) const;
    ayni_status_t load(const ayni_context_info_v1 &);
    ayni_status_t restore_turn();
    ayni_status_t save();
    ayni_status_t confirm(const strategies::ConversationRecord &);
    bool confirmation_matches(const strategies::ConversationRecord &) const;
    ayni_status_t saved(const ayni_context_info_v1 &);
    ayni_status_t turn(const ayni_context_info_v1 &, uint64_t durable);
    ayni_status_t cancel_turn(uint64_t durable);
    ayni_status_t discard();
    ayni_status_t unfinished();
    ayni_status_t settled(uint64_t durable) const;
    ayni_status_t collect_superseded(const strategies::ConversationRecord &);
    ayni_status_t prepare_publication(const ayni_context_info_v1 &);
    ayni_status_t publish(const ayni_context_info_v1 &);
    ayni_status_t erase(const ayni_context_info_v1 &);
    ayni_status_t input();
    struct Confirmation {
        uint64_t generation = 0, revision = 0; // Privacy/source versions; zero before admission.
        std::array<strategies::BodyRef, 4> roots{}; // Exact immutable source graph.
        std::vector<strategies::BodyRef> refs;      // Complete dependency set, walked only once.
        size_t durable = 0; // refs[0..durable) confirmed; remaining suffix still pending.
    };
    std::optional<strategies::ConversationRecord>
        superseded_; // Exact old graph retained until deletion drains.
    std::optional<Confirmation>
        confirmed_; // Graph confirmation progress/proof only; all privacy gates still read Core.
    Config config_{};
    Phase phase_ = Phase::empty;      // Sole lifecycle/worker-buffer state.
    Save save_ = Save::initialize;    // Meaningful only in save/saving.
    ayni_status_t failure_ = AYNI_OK; // Sticky classified error only in failed.
    uint64_t context_id_ = 0;         // Stable public anchor identity after configure.
    uint64_t value_version_ = 0;      // AYFV CAS version, independent of context generation.
    uint64_t goal_ = 0;               // Current/last turn Goal; retained until next accepted turn.
    strategies::ConversationRecord current_{}, candidate_{}; // Adopt only proven versions.
    strategies::Encoded file_{}, parameters_{}; // Worker-exclusive file lane; owner parameter lane.
    std::string input_;                         // Accepted user text until its body is durable.
    uint64_t memory_id_ = 0;
    enum class EndRequest { none, cancel, abandon };
    EndRequest end_request_ = EndRequest::none; // Owner intent, never Core lifecycle state.
    bool held_ = false;                         // Last poll's public Task hold observation.
    std::optional<TurnResult> result_;          // Delivered once; history itself remains queryable.

    hosts::storage::FileValueResult answer_{}; // Published before completed_'s release store.
    std::atomic<bool> completed_{false}; // Worker->owner one-shot publication, not domain state.
};
} // namespace ayni::runtime::conversation
#endif
