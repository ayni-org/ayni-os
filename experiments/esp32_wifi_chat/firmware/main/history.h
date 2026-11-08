// The experiment's outer conversation store for the built-in ToolLoop (builtin.h): one record
// value file on the Host's value port, and the bodies it names in the shared body store.
//
// Only a completed turn changes the record. A turn that fails or is cancelled leaves the last
// saved record and its input body is dropped; one cut by a reset is not in the record either
// (its input file stays until the volume is erased). So neither the next turn nor the next boot
// replays an unfinished request (the user's 2026-11-01 session choice). An answer is handed
// out only after the record that names it is durable. Owner task only; the file completion
// runs on the storage worker, which wakes the owner.
#pragma once
#include "body_cache.h"
#include <atomic>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/strategies/builtin.h>
#include <string>
namespace wifichat {
class History {
  public:
    History() = default;
    History(const History &) = delete;
    History &operator=(const History &) = delete;
    // Starts loading the record of owner (the Agent instance). All borrows outlive Host join.
    ayni_status_t configure(const ayni_workspace_v1 *workspace, BodyCache *bodies,
                            ayni::hosts::storage::FileValuePort files, uint64_t owner,
                            uint64_t record_id = UINT64_C(0x5553424348415431));
    // One queue operation or state change; never waits. A failure is sticky.
    ayni_status_t poll();
    bool ready() const { return phase_ == Phase::ready && !answer_pending_; }
    // Stores the input body and writes the turn's Goal parameters. NOT_READY until the body
    // is durable: call again with the same text.
    ayni_status_t begin(ayni_string_v1 user, ayni::strategies::Encoded &parameters);
    // A completed turn's checkpoint becomes the next record; its answer follows once saved.
    ayni_status_t finish(ayni_bytes_v1 checkpoint);
    // The turn ended without an answer: release the bodies the record does not name.
    void abandon();
    // Drops every round and the summary, saved like a turn ("clear context").
    ayni_status_t forget();
    // The latest answer, once, after its record is durable.
    bool take_answer(std::string &out);
    uint64_t rounds() const { return current_.last_round; }

  private:
    enum class Phase { empty, load, loading, ready, save, saving, failed };
    static void complete(void *, const ayni::hosts::storage::FileValueResult &) noexcept;
    ayni::strategies::ConversationRecord fresh(uint64_t generation) const;
    ayni_status_t loaded();
    ayni_status_t saved();
    ayni_status_t fail(ayni_status_t status);
    void collect();
    void release_superseded(const ayni::strategies::ConversationRecord &old);
    const ayni_workspace_v1 *workspace_ = nullptr;
    BodyCache *bodies_ = nullptr;
    ayni::hosts::storage::FileValuePort files_{};
    uint64_t owner_ = 0;
    uint64_t record_id_ = 0;
    Phase phase_ = Phase::empty;
    ayni_status_t failure_ = AYNI_OK;
    uint64_t value_version_ = 0; // The record file's CAS generation; 0 while none exists.
    bool answer_pending_ = false, answer_ready_ = false, turn_save_ = false;
    std::string answer_;
    ayni::strategies::ConversationRecord current_{}, candidate_{};
    ayni::strategies::Encoded file_{}; // Worker-exclusive while a file request is in flight.
    ayni::hosts::storage::FileValueResult result_{}; // Published before completed_.
    std::atomic<bool> completed_{false};
};
} // namespace wifichat
