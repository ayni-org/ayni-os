#pragma once
#include <ayni/ayni.hpp>
#include <ayni/bodies.h>
#include <ayni/commands.hpp>
#include <ayni/context.h>
#include <string>
namespace wifichat {
// Follow-up speech uses its own parameterized Rules Goal, the existing speaker capability,
// and Core's call ledger. Saved conversation text remains independent of playback success.
class ReplyVoice {
  public:
    static constexpr uint32_t version = 1;
    static constexpr const char *type_id = "wifi.reply.voice";
    ayni_strategy_v1 strategy(const ayni_body_store_v1 *);
    static ayni_goal_type_v1 goal_type();
    ayni_status_t begin(std::string answer, const ayni_context_info_v1 &source);
    void set_accepting(bool value) { accepting_ = value; }
    void cancel() { cancelling_ = true; }
    // BUSY retains state; every other status is terminal after the Goal has settled durably.
    ayni_status_t advance(ayni_kernel_t *, ayni::CommandClient &, uint64_t session, uint64_t owner,
                          uint64_t durable);
    bool active() const { return !text_.empty(); }
    uint64_t goal() const { return goal_; }
    ayni_status_t failure_status() const { return failure_; }
    void reset();

  private:
    ayni_status_t prepare_chunk(ayni_kernel_t *, ayni::CommandClient &, uint64_t session,
                                uint64_t owner);
    ayni_status_t poll_goal(ayni_kernel_t *, ayni::CommandClient &, uint64_t durable);
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static ayni_status_t validate(void *, ayni_bytes_v1);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    std::string text_; // Owned complete answer; never logged or moved while a chunk is borrowed.
    const ayni_body_store_v1 *bodies_ = nullptr; // Borrowed stable port through Core/Host close.
    ayni_body_ref_v1 body_{}; // Current durable chunk, released only after its Goal ends.
    bool accepting_ = false;  // Owner opens after restored Goals have been cancelled.
    size_t offset_ = 0;
    std::optional<uint32_t> chunk_; // Present from write preparation through durable Goal end.
    struct Source {
        uint64_t id, owner, generation;
        uint32_t revision;
    };
    std::optional<Source> source_; // Original published-answer basis, pinned across all segments.
    uint64_t goal_ = 0; // None until an accepted create; retained through durable termination.
    bool cancelling_ = false;
    ayni_status_t failure_ = AYNI_OK; // Original failure, retained while cancellation drains.
};
} // namespace wifichat
