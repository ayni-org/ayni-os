// Portable conversation/file acknowledgement application. No platform selection or I/O loop.
#ifndef AYNI_RUNTIME_CONVERSATION_H
#define AYNI_RUNTIME_CONVERSATION_H
#include <atomic>
#include <ayni/ayni.hpp>
#include <ayni/context.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/strategies/builtin.h>
#include <string>
namespace ayni::runtime::conversation {
// Composition-owned command identities/session. submit copies body synchronously;
// no callback may reenter Session. The caller observes its own command receipts.
struct Commands {
    void *context = nullptr;
    Submission (*submit)(void *, ayni_intent_kind_t, const void *, uint32_t) = nullptr;
};
struct Config {
    ayni_kernel_t *kernel = nullptr; // Borrowed through shutdown/Host join.
    uint64_t session = 0;            // Bound owner's MANAGEMENT session this boot.
    uint64_t owner_instance = 0;     // Exact Agent owner; context must already be opened/durable.
    ayni_string_v1 goal_type{}; // Borrowed registered ToolLoop GoalType id through destruction.
    const ayni_workspace_v1 *workspace =
        nullptr;                         // Exclusive during each owner call, may be shared.
    hosts::storage::FileValuePort files; // Worker callback context remains alive through Host join.
    FileBodies *bodies = nullptr;        // The ToolLoop's body store, borrowed through Host join.
    Commands commands;
};
// Caller places this bounded object in suitable RAM (it contains large records).
// Owner-thread, no reentry. poll performs bounded public-API/queue work, never waits.
// After any accepted file request, this object/buffers live until Host close joins.
// A clean existing anchor loads its exact file; unfinished cross-store recovery
// is explicitly NOT_READY and preserved, never silently initialized or replayed.
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
    // OK copies input and starts storing it as a body, then the durable draft save. One turn at
    // a time per Session.
    // Completion is ready() with a new last_round; the final answer crosses Core ACK durability.
    ayni_status_t begin(ayni_string_v1 user, uint64_t memory_id = 0);
    ayni_status_t forget(ayni_context_action_t action, uint64_t turn = 0);
    // Rechecks the current Core barrier, including owner changes since the last poll.
    bool ready() const;
    // Borrow only until the next owner operation/Kernel mutation. Each lookup
    // rechecks privacy; retaining a pointer is not native-process isolation.
    const strategies::ConversationRecord *record() const { return ready() ? &current_ : nullptr; }
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
        publish,
        acknowledge,
        acknowledged,
        erase,
        failed
    };
    enum class Save { initialize, draft, publication, erasure };
    static void complete(void *, const hosts::storage::FileValueResult &) noexcept;
    ayni_status_t context(ayni_context_info_v1 &) const;
    ayni_status_t intake(ayni_context_info_v1 &) const;
    Submission command(ayni_intent_kind_t, const ayni_context_info_v1 &, uint64_t task = 0,
                       ayni_context_action_t action = AYNI_CONTEXT_IDLE, uint64_t turn = 0);
    bool matches(const strategies::ConversationRecord &, const ayni_context_info_v1 &) const;
    ayni_status_t load(const ayni_context_info_v1 &);
    ayni_status_t save();
    ayni_status_t saved(const ayni_context_info_v1 &);
    ayni_status_t turn(const ayni_context_info_v1 &);
    ayni_status_t publish(const ayni_context_info_v1 &);
    ayni_status_t erase(const ayni_context_info_v1 &);
    ayni_status_t input();
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
    hosts::storage::FileValueResult answer_{}; // Published before completed_'s release store.
    std::atomic<bool> completed_{false}; // Worker->owner one-shot publication, not domain state.
};
} // namespace ayni::runtime::conversation
#endif
