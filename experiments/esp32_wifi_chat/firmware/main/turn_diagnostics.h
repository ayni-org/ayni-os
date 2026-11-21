// Platform-independent timing accumulators for ESP32 Wi-Fi Chat diagnostics.
// Captures monotonic timestamps and Host I/O snapshots in fixed storage with zero window logging.
#pragma once
#include <array>
#include <ayni/hosts/esp_idf/metrics.h>
#include <ayni/runtime/conversation.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace wifichat {

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
    uint64_t foreground_measures = 0, foreground_measure_us = 0;
    uint64_t idle_measures = 0, idle_measure_us = 0;
    ayni::hosts::esp_idf::ValueIoMetrics values{};

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
        d.foreground_measures = after.foreground_measures - before.foreground_measures;
        d.foreground_measure_us = after.foreground_measure_us - before.foreground_measure_us;
        d.idle_measures = after.idle_measures - before.idle_measures;
        d.idle_measure_us = after.idle_measure_us - before.idle_measure_us;
        for (size_t i = 0; i < d.values.size(); ++i) {
            d.values[i] = after.values[i].since(before.values[i]);
        }
        return d;
    }
};

// Fixed-size sampler for the 7 boundaries from final model_result_posted -> answer_saved.
// Sampling only copies monotonic time and Host I/O counters; formatting happens after the window.
class PublicationTimeline {
  public:
    using Stage = ayni::runtime::conversation::PublicationStage;
    using Metrics = ayni::hosts::esp_idf::StorageMetrics;
    static constexpr size_t max_samples = 7, max_segments = max_samples - 1U;
    static constexpr size_t report_line_capacity = 384;

    enum class Boundary : uint8_t {
        model_result_posted = 0,
        checkpoint_ready = 1,
        publish_durable = 2,
        record_saved = 3,
        collected = 4,
        context_ready = 5,
        answer_saved = 6
    };
    enum class Order : uint8_t { unknown = 0, async = 1, direct = 2 };

    // Coherent snapshot taken at one boundary. All durations/counters are monotonic cumulative.
    struct Sample {
        Boundary boundary = Boundary::model_result_posted;
        uint64_t at_us = 0; // Monotonic timestamp at this boundary.
        Metrics metrics{};  // Host storage counters sampled at the same instant.
    };
    // Adjacent boundary diff in actual execution order (from -> to).
    struct Segment {
        Boundary from = Boundary::model_result_posted;
        Boundary to = Boundary::answer_saved;
        uint64_t start_us = 0, end_us = 0;
        StoragePhaseDelta io{};
    };
    struct Summary {
        Order order = Order::unknown;
        uint64_t turn = 0, goal = 0, task = 0;
        uint64_t start_us = 0, end_us = 0;
        std::array<Segment, max_segments> segments{};
        size_t segment_count = 0;
        size_t longest_index = 0;  // Segment index with maximum wall_us; valid when count > 0.
        StoragePhaseDelta total{}; // Direct head-to-tail diff from first sample to last sample.
        bool complete = false;
    };

    static const char *boundary_name(Boundary boundary) {
        switch (boundary) {
        case Boundary::model_result_posted:
            return "model_result_posted";
        case Boundary::checkpoint_ready:
            return "checkpoint_ready";
        case Boundary::publish_durable:
            return "publish_durable";
        case Boundary::record_saved:
            return "record_saved";
        case Boundary::collected:
            return "collected";
        case Boundary::context_ready:
            return "context_ready";
        case Boundary::answer_saved:
            return "answer_saved";
        }
        return "unknown";
    }
    static Boundary boundary_of(Stage stage) {
        switch (stage) {
        case Stage::checkpoint_ready:
            return Boundary::checkpoint_ready;
        case Stage::publish_durable:
            return Boundary::publish_durable;
        case Stage::record_saved:
            return Boundary::record_saved;
        case Stage::collected:
            return Boundary::collected;
        case Stage::context_ready:
            return Boundary::context_ready;
        }
        return Boundary::checkpoint_ready;
    }
    static const char *stage_name(Stage stage) { return boundary_name(boundary_of(stage)); }
    static const char *order_name(Order order) {
        return order == Order::async ? "async" : order == Order::direct ? "direct" : "unknown";
    }

    void reset(uint64_t turn = 0) {
        turn_ = turn;
        goal_ = task_ = 0;
        has_model_posted_ = false;
        count_ = 0;
        order_ = Order::unknown;
        seen_ = 0;
    }

    // Overwrites slot 0 while no Session publication stage has fired, retaining the final model
    // result on multi-round tool turns.
    void mark_model_posted(uint64_t at_us, const Metrics &metrics) {
        if (seen_ != 0U) {
            return;
        }
        samples_[0] = {Boundary::model_result_posted, at_us, metrics};
        has_model_posted_ = true;
        count_ = 1;
    }

    // Records each Session publication boundary at most once in actual occurrence order.
    bool record_stage(uint64_t goal, uint64_t task, Stage stage, uint64_t at_us,
                      const Metrics &metrics) {
        const uint8_t bit = static_cast<uint8_t>(1U << static_cast<size_t>(stage));
        if ((seen_ & bit) != 0U || count_ >= max_samples) {
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
        samples_[count_++] = {boundary_of(stage), at_us, metrics};
        return true;
    }

    // Records the final answer_saved boundary at most once.
    bool record_answer_saved(uint64_t at_us, const Metrics &metrics) {
        if ((seen_ & answer_saved_bit) != 0U || count_ >= max_samples) {
            return false;
        }
        seen_ |= answer_saved_bit;
        samples_[count_++] = {Boundary::answer_saved, at_us, metrics};
        return true;
    }

    bool has_session_stage() const { return (seen_ & session_stage_mask) != 0U; }
    bool complete() const {
        return has_model_posted_ && seen_ == all_mask && order_ != Order::unknown &&
               count_ == max_samples;
    }
    size_t sample_count() const { return count_; }
    size_t segment_count() const { return count_ > 1U ? count_ - 1U : 0U; }
    const Sample &sample(size_t index) const { return samples_[index]; }

    Segment segment_at(size_t index) const {
        const auto &a = samples_[index];
        const auto &b = samples_[index + 1U];
        return {a.boundary, b.boundary, a.at_us, b.at_us,
                StoragePhaseDelta::between(a.metrics, b.metrics, delta(a.at_us, b.at_us))};
    }

    StoragePhaseDelta total_delta() const {
        if (count_ < 2U) {
            return {};
        }
        const auto &first = samples_[0];
        const auto &last = samples_[count_ - 1U];
        return StoragePhaseDelta::between(first.metrics, last.metrics,
                                          delta(first.at_us, last.at_us));
    }

    Summary summary() const {
        Summary out{};
        out.order = order_;
        out.turn = turn_;
        out.goal = goal_;
        out.task = task_;
        out.complete = complete();
        out.segment_count = segment_count();
        if (count_ >= 1U) {
            out.start_us = samples_[0].at_us;
            out.end_us = samples_[count_ - 1U].at_us;
        }
        for (size_t i = 0; i < out.segment_count; ++i) {
            out.segments[i] = segment_at(i);
            if (out.segments[i].io.wall_us > out.segments[out.longest_index].io.wall_us) {
                out.longest_index = i;
            }
        }
        out.total = total_delta();
        return out;
    }

    // Formats one adjacent publication segment line into a bounded caller buffer.
    static int format_segment(uint64_t turn, size_t index, size_t total_segments,
                              const Segment &seg, char *out, size_t capacity) {
        return std::snprintf(
            out, capacity,
            "wifi pub turn=%llu seg=%zu/%zu from=%s to=%s wall_us=%llu queue_us=%llu "
            "io_us=%llu append=%llu/%llu vread=%llu/%llu vwrite=%llu/%llu vremove=%llu/%llu "
            "lfs_size=%llu/%llu\n",
            static_cast<unsigned long long>(turn), index + 1U, total_segments,
            boundary_name(seg.from), boundary_name(seg.to),
            static_cast<unsigned long long>(seg.io.wall_us),
            static_cast<unsigned long long>(seg.io.queue_us),
            static_cast<unsigned long long>(seg.io.io_us),
            static_cast<unsigned long long>(seg.io.append_calls),
            static_cast<unsigned long long>(seg.io.append_us),
            static_cast<unsigned long long>(seg.io.value_read_calls),
            static_cast<unsigned long long>(seg.io.value_read_us),
            static_cast<unsigned long long>(seg.io.value_write_calls),
            static_cast<unsigned long long>(seg.io.value_write_us),
            static_cast<unsigned long long>(seg.io.value_remove_calls),
            static_cast<unsigned long long>(seg.io.value_remove_us),
            static_cast<unsigned long long>(seg.io.lfs_size_calls),
            static_cast<unsigned long long>(seg.io.lfs_size_us));
    }

    // Formats the publication summary line with total I/O and the single longest segment.
    static int format_summary(uint64_t turn, uint64_t goal, uint64_t task, Order order,
                              bool is_complete, const StoragePhaseDelta &total,
                              const Segment &longest, char *out, size_t capacity) {
        return std::snprintf(
            out, capacity,
            "wifi pub_summary turn=%llu goal=%llu task=%llu order=%s complete=%u total_us=%llu "
            "max=%s->%s:%llu queue_us=%llu io_us=%llu append=%llu/%llu vread=%llu/%llu "
            "vwrite=%llu/%llu vremove=%llu/%llu lfs_size=%llu/%llu\n",
            static_cast<unsigned long long>(turn), static_cast<unsigned long long>(goal),
            static_cast<unsigned long long>(task), order_name(order), unsigned(is_complete),
            static_cast<unsigned long long>(total.wall_us), boundary_name(longest.from),
            boundary_name(longest.to), static_cast<unsigned long long>(longest.io.wall_us),
            static_cast<unsigned long long>(total.queue_us),
            static_cast<unsigned long long>(total.io_us),
            static_cast<unsigned long long>(total.append_calls),
            static_cast<unsigned long long>(total.append_us),
            static_cast<unsigned long long>(total.value_read_calls),
            static_cast<unsigned long long>(total.value_read_us),
            static_cast<unsigned long long>(total.value_write_calls),
            static_cast<unsigned long long>(total.value_write_us),
            static_cast<unsigned long long>(total.value_remove_calls),
            static_cast<unsigned long long>(total.value_remove_us),
            static_cast<unsigned long long>(total.lfs_size_calls),
            static_cast<unsigned long long>(total.lfs_size_us));
    }

    // Streams the segment lines and summary line without placing a full Summary on the stack.
    void emit_report(void *ctx, void (*emit)(void *ctx, const char *line, size_t size)) const {
        const size_t segments = segment_count();
        if (!emit || segments == 0U) {
            return;
        }
        char line[report_line_capacity]{};
        Segment longest{};
        bool have_longest = false;
        for (size_t i = 0; i < segments; ++i) {
            const Segment seg = segment_at(i);
            if (!have_longest || seg.io.wall_us > longest.io.wall_us) {
                longest = seg;
                have_longest = true;
            }
            const int written = format_segment(turn_, i, segments, seg, line, sizeof(line));
            if (written > 0 && static_cast<size_t>(written) < sizeof(line)) {
                emit(ctx, line, static_cast<size_t>(written));
            }
            emit_values(i, seg.io, ctx, emit);
            emit_space(i + 1U, seg.io, ctx, emit);
        }
        const StoragePhaseDelta total = total_delta();
        emit_space(0, total, ctx, emit);
        const int written = format_summary(turn_, goal_, task_, order_, complete(), total, longest,
                                           line, sizeof(line));
        if (written > 0 && static_cast<size_t>(written) < sizeof(line)) {
            emit(ctx, line, static_cast<size_t>(written));
        }
    }

    uint64_t turn() const { return turn_; }
    uint64_t goal() const { return goal_; }
    uint64_t task() const { return task_; }
    Order order() const { return order_; }

  private:
    // Segment zero reports the whole model-result-to-answer window.
    void emit_space(size_t segment, const StoragePhaseDelta &io, void *ctx,
                    void (*emit)(void *, const char *, size_t)) const {
        if (!(io.foreground_measures || io.idle_measures)) {
            return;
        }
        const auto n = [](uint64_t value) { return static_cast<unsigned long long>(value); };
        char line[report_line_capacity];
        const auto written =
            std::snprintf(line, sizeof(line),
                          "wifi pub_space turn=%llu seg=%zu foreground=%llu/%llu "
                          "idle=%llu/%llu\n",
                          n(turn_), segment, n(io.foreground_measures), n(io.foreground_measure_us),
                          n(io.idle_measures), n(io.idle_measure_us));
        if (written > 0 && static_cast<size_t>(written) < sizeof(line)) {
            emit(ctx, line, static_cast<size_t>(written));
        }
    }

    void emit_values(size_t segment, const StoragePhaseDelta &io, void *ctx,
                     void (*emit)(void *, const char *, size_t)) const {
        constexpr std::array roles{"other", "body_chunk", "owner_manifest", "conversation_root"};
        static_assert(roles.size() == ayni::hosts::esp_idf::ValueIoMetrics{}.size());
        using Op = ayni::hosts::esp_idf::ValueOperationMetrics;
        const auto count = [](const Op &op) { return static_cast<unsigned long long>(op.calls); };
        const auto bytes = [](const Op &op) { return static_cast<unsigned long long>(op.bytes); };
        const auto service = [](const Op &op) {
            return static_cast<unsigned long long>(op.service_us);
        };
        const auto complete = [](const Op &op) {
            return static_cast<unsigned long long>(op.completion_us);
        };
        char line[report_line_capacity];
        for (size_t i = 0; i < roles.size(); ++i) {
            const auto &[read, write, remove] = io.values[i];
            if (!(read.calls || write.calls || remove.calls)) {
                continue;
            }
            const auto written =
                std::snprintf(line, sizeof(line),
                              "wifi pub_value turn=%llu seg=%zu role=%s read=%llu/%llu/%llu/%llu "
                              "write=%llu/%llu/%llu/%llu remove=%llu/%llu/%llu/%llu\n",
                              static_cast<unsigned long long>(turn_), segment + 1U, roles[i],
                              count(read), bytes(read), service(read), complete(read), count(write),
                              bytes(write), service(write), complete(write), count(remove),
                              bytes(remove), service(remove), complete(remove));
            if (written > 0 && static_cast<size_t>(written) < sizeof(line)) {
                emit(ctx, line, static_cast<size_t>(written));
            }
        }
    }

    static constexpr size_t stage_count = 5;
    static constexpr uint8_t session_stage_mask = static_cast<uint8_t>((1U << stage_count) - 1U);
    static constexpr uint8_t answer_saved_bit = static_cast<uint8_t>(1U << stage_count);
    static constexpr uint8_t all_mask = static_cast<uint8_t>((1U << (stage_count + 1U)) - 1U);

    static constexpr uint64_t delta(uint64_t earlier, uint64_t later) {
        return later >= earlier ? later - earlier : 0;
    }

    uint64_t turn_ = 0, goal_ = 0, task_ = 0;
    bool has_model_posted_ = false;
    std::array<Sample, max_samples> samples_{};
    size_t count_ = 0;
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

} // namespace wifichat
