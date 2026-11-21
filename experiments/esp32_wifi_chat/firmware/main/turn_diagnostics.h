// Platform-independent timing accumulators for ESP32 Wi-Fi Chat diagnostics.
// Tracks one turn's publication boundaries, model handoff timestamps and storage phase deltas.
#pragma once
#include <array>
#include <ayni/hosts/esp_idf/metrics.h>
#include <ayni/runtime/conversation.h>
#include <cstddef>
#include <cstdint>

namespace wifichat {

// Records the 6 monotonic publication boundaries from final model_result_posted -> answer_saved.
// Each boundary is captured at most once; retries inside a phase stay within that segment.
class PublicationTimeline {
  public:
    using Stage = ayni::runtime::conversation::PublicationStage;
    enum class Order : uint8_t { unknown = 0, async = 1, direct = 2 };
    struct Step {
        uint64_t at_us = 0;      // Monotonic sample at this boundary.
        uint64_t seg_us = 0;     // Wall-clock delta from the previous recorded boundary.
        uint64_t elapsed_us = 0; // Wall-clock delta from final model_result_posted.
    };
    struct Summary {
        Order order = Order::unknown;
        uint64_t turn = 0, goal = 0, task = 0;
        uint64_t total_us = 0; // answer_saved_us - model_posted_us.
        // Adjacent segments in actual execution order:
        // async:  [0] model->checkpoint, [1] checkpoint->publish, [2] publish->record,
        //         [3] record->collected, [4] collected->context,  [5] context->answer.
        // direct: [0] model->publish,    [1] publish->checkpoint, [2] checkpoint->record,
        //         [3] record->collected, [4] collected->context,  [5] context->answer.
        std::array<uint64_t, 6> segments_us{};
        bool complete = false;
    };

    static const char *stage_name(Stage stage) {
        switch (stage) {
        case Stage::checkpoint_ready:
            return "checkpoint_ready";
        case Stage::publish_durable:
            return "publish_durable";
        case Stage::record_saved:
            return "record_saved";
        case Stage::collected:
            return "collected";
        case Stage::context_ready:
            return "context_ready";
        }
        return "unknown";
    }
    static const char *order_name(Order order) {
        return order == Order::async ? "async" : order == Order::direct ? "direct" : "unknown";
    }

    void reset(uint64_t turn = 0) {
        turn_ = turn;
        goal_ = task_ = 0;
        has_model_posted_ = false;
        model_posted_us_ = last_us_ = answer_saved_us_ = 0;
        stage_at_.fill(0);
        order_ = Order::unknown;
        seen_ = 0;
    }

    // Overwrites while no publication boundary has fired yet, retaining the final model post.
    void mark_model_posted(uint64_t at_us) {
        if (seen_ != 0U) {
            return;
        }
        has_model_posted_ = true;
        model_posted_us_ = at_us;
        last_us_ = at_us;
    }

    bool record_stage(uint64_t goal, uint64_t task, Stage stage, uint64_t at_us, Step &out) {
        const auto index = stage_index(stage);
        const uint8_t bit = static_cast<uint8_t>(1U << index);
        if ((seen_ & bit) != 0U) {
            return false;
        }
        seen_ |= bit;
        if (goal) {
            goal_ = goal;
        }
        if (task) {
            task_ = task;
        }
        if (order_ == Order::unknown) {
            if (stage == Stage::checkpoint_ready) {
                order_ = Order::async;
            } else if (stage == Stage::publish_durable) {
                order_ = Order::direct;
            }
        }
        const uint64_t prev = last_us_ ? last_us_ : at_us;
        const uint64_t anchor = has_model_posted_ ? model_posted_us_ : at_us;
        out = {at_us, delta(prev, at_us), delta(anchor, at_us)};
        stage_at_[index] = at_us;
        last_us_ = at_us;
        return true;
    }

    bool record_answer_saved(uint64_t at_us, Step &out) {
        if ((seen_ & answer_saved_bit) != 0U) {
            return false;
        }
        seen_ |= answer_saved_bit;
        const uint64_t prev = last_us_ ? last_us_ : at_us;
        const uint64_t anchor = has_model_posted_ ? model_posted_us_ : at_us;
        out = {at_us, delta(prev, at_us), delta(anchor, at_us)};
        answer_saved_us_ = at_us;
        last_us_ = at_us;
        return true;
    }

    Summary summary() const {
        Summary out{};
        out.order = order_;
        out.turn = turn_;
        out.goal = goal_;
        out.task = task_;
        out.complete = has_model_posted_ && seen_ == all_mask && order_ != Order::unknown;
        if (!out.complete) {
            return out;
        }
        const uint64_t first = order_ == Order::async
                                   ? stage_at_[stage_index(Stage::checkpoint_ready)]
                                   : stage_at_[stage_index(Stage::publish_durable)];
        const uint64_t second = order_ == Order::async
                                    ? stage_at_[stage_index(Stage::publish_durable)]
                                    : stage_at_[stage_index(Stage::checkpoint_ready)];
        const std::array<uint64_t, 7> points{
            model_posted_us_,
            first,
            second,
            stage_at_[stage_index(Stage::record_saved)],
            stage_at_[stage_index(Stage::collected)],
            stage_at_[stage_index(Stage::context_ready)],
            answer_saved_us_,
        };
        for (size_t at = 0; at < out.segments_us.size(); ++at) {
            out.segments_us[at] = delta(points[at], points[at + 1]);
        }
        out.total_us = delta(points.front(), points.back());
        return out;
    }

    uint64_t turn() const { return turn_; }
    uint64_t goal() const { return goal_; }
    uint64_t task() const { return task_; }
    uint64_t model_posted_us() const { return model_posted_us_; }

  private:
    static constexpr size_t stage_count = 5;
    static constexpr uint8_t answer_saved_bit = static_cast<uint8_t>(1U << stage_count);
    static constexpr uint8_t all_mask = static_cast<uint8_t>((1U << (stage_count + 1U)) - 1U);

    static constexpr size_t stage_index(Stage stage) { return static_cast<size_t>(stage); }
    static constexpr uint64_t delta(uint64_t earlier, uint64_t later) {
        return later >= earlier ? later - earlier : 0;
    }

    uint64_t turn_ = 0, goal_ = 0, task_ = 0;
    bool has_model_posted_ = false;
    uint64_t model_posted_us_ = 0, last_us_ = 0, answer_saved_us_ = 0;
    std::array<uint64_t, stage_count> stage_at_{};
    Order order_ = Order::unknown;
    uint8_t seen_ = 0;
};

// Monotonic timestamps of the most recent UI frame rendered on chat-io.
// Because Runtime::run() drains results() before draw()/paint(), any model result enqueued into
// done_ before paint_end_us waited for the remainder (or entirety) of that frame.
struct UiFrameWindow {
    enum class Overlap : uint8_t { idle = 0, before_draw = 1, in_draw = 2, in_paint = 3 };

    uint64_t draw_begin_us = 0, draw_end_us = 0;
    uint64_t paint_begin_us = 0, paint_end_us = 0;

    static const char *name(Overlap overlap) {
        switch (overlap) {
        case Overlap::idle:
            return "idle";
        case Overlap::before_draw:
            return "before_draw";
        case Overlap::in_draw:
            return "in_draw";
        case Overlap::in_paint:
            return "in_paint";
        }
        return "idle";
    }

    Overlap classify(uint64_t done_enqueued_us, uint64_t &frame_wait_us) const {
        if (!paint_end_us || !done_enqueued_us || done_enqueued_us >= paint_end_us) {
            frame_wait_us = 0;
            return Overlap::idle;
        }
        if (done_enqueued_us < draw_begin_us) {
            frame_wait_us = paint_end_us - draw_begin_us;
            return Overlap::before_draw;
        }
        frame_wait_us = paint_end_us - done_enqueued_us;
        return done_enqueued_us < draw_end_us ? Overlap::in_draw : Overlap::in_paint;
    }
};

// Per-phase deltas of ESP Host storage metrics. wall_us is the phase's wall clock; queue_us and
// io_us sum across concurrent queued/served work items and therefore may overlap each other.
struct StoragePhaseDelta {
    uint64_t wall_us = 0;
    uint64_t completed = 0, queue_us = 0, io_us = 0, space_us = 0, post_us = 0, idle_space_us = 0;
    uint64_t append_calls = 0, append_us = 0, value_us = 0;
    uint64_t value_read_calls = 0, value_read_us = 0;
    uint64_t value_write_calls = 0, value_write_us = 0;
    uint64_t value_remove_calls = 0, value_remove_us = 0;
    uint64_t scan_frames = 0;
    uint64_t file_read_calls = 0, file_read_bytes = 0;
    uint64_t file_read_us = 0, file_write_us = 0, file_sync_us = 0;
    uint64_t lfs_size_calls = 0, lfs_size_us = 0;

    static StoragePhaseDelta between(const ayni::hosts::esp_idf::StorageMetrics &before,
                                     const ayni::hosts::esp_idf::StorageMetrics &after,
                                     uint64_t wall_us) {
        StoragePhaseDelta d{};
        d.wall_us = wall_us;
        d.completed = after.completed - before.completed;
        d.queue_us = after.queue_us - before.queue_us;
        d.io_us = after.io_us - before.io_us;
        d.space_us = after.space_us - before.space_us;
        d.post_us = after.post_us - before.post_us;
        d.idle_space_us = after.idle_space_us - before.idle_space_us;
        d.append_calls = after.append_calls - before.append_calls;
        d.append_us = after.append_us - before.append_us;
        d.value_us = after.value_us - before.value_us;
        d.value_read_calls = after.value_read_calls - before.value_read_calls;
        d.value_read_us = after.value_read_us - before.value_read_us;
        d.value_write_calls = after.value_write_calls - before.value_write_calls;
        d.value_write_us = after.value_write_us - before.value_write_us;
        d.value_remove_calls = after.value_remove_calls - before.value_remove_calls;
        d.value_remove_us = after.value_remove_us - before.value_remove_us;
        d.scan_frames = after.scan_frames - before.scan_frames;
        d.file_read_calls = after.files.read_calls - before.files.read_calls;
        d.file_read_bytes = after.files.read_bytes - before.files.read_bytes;
        d.file_read_us = after.files.read_us - before.files.read_us;
        d.file_write_us = after.files.write_us - before.files.write_us;
        d.file_sync_us = after.files.sync_us - before.files.sync_us;
        d.lfs_size_calls = after.files.space_measure_calls - before.files.space_measure_calls;
        d.lfs_size_us = after.files.space_measure_us - before.files.space_measure_us;
        return d;
    }
};

} // namespace wifichat
