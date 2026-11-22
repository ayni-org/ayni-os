#pragma once
#include "output_intervals.h"
#include <array>
#include <ayni/hosts/esp_idf/work_history.h>

namespace wifichat {
// A model window may contain several sequential physical calls. Keep the gaps
// distinct from active output, and mark the split incomplete if boundaries are missing.
struct ToolIoWindow {
    using Sample = ayni::hosts::esp_idf::StorageWorkSample;
    using Role = ayni::hosts::storage::FileValueRole;
    enum Phase { before, active, between, after, phase_count };
    struct Operation {
        uint64_t count = 0, us = 0;
        void add(uint64_t service_us) {
            ++count;
            us += service_us;
        }
    };
    struct Part {
        uint64_t wall_us = 0, queue_us = 0;
        Operation io, append, read, write, remove;
        std::array<Operation, static_cast<size_t>(Role::count)> writes;

        void add(const Sample &sample) {
            queue_us += sample.queue_us;
            io.add(sample.service_us);
            switch (sample.kind) {
            case Sample::Kind::append:
                append.add(sample.service_us);
                break;
            case Sample::Kind::read:
                read.add(sample.service_us);
                break;
            case Sample::Kind::write:
                write.add(sample.service_us);
                writes[static_cast<size_t>(sample.role)].add(sample.service_us);
                break;
            case Sample::Kind::remove:
                remove.add(sample.service_us);
                break;
            case Sample::Kind::other:
                break;
            }
        }
    };
    std::array<Part, phase_count> parts{};

    // Request identities bound the same delta as StorageMetrics::completed. At a motor
    // timestamp, a completion exactly at start belongs to active and at stop to after.
    template <class Read>
    bool collect(uint64_t from_us, uint64_t to_us, const OutputIntervals &output, uint64_t first,
                 uint64_t next, Read read) {
        uint64_t previous_stop = from_us;
        size_t count = 0;
        for (const auto &interval : output) {
            if (!interval.call) {
                break;
            }
            if (interval.start_us < previous_stop || interval.stop_us < interval.start_us ||
                interval.stop_us > to_us) {
                return false;
            }
            parts[count ? between : before].wall_us += interval.start_us - previous_stop;
            parts[active].wall_us += interval.stop_us - interval.start_us;
            previous_stop = interval.stop_us;
            ++count;
        }
        if (to_us < previous_stop || next < first ||
            next - first > ayni::hosts::esp_idf::StorageWorkHistory::capacity) {
            return false;
        }
        parts[count ? after : before].wall_us += to_us - previous_stop;
        for (auto i = first; i < next; ++i) {
            Sample sample;
            if (!read(i, sample)) {
                return false;
            }
            Phase phase = before;
            for (size_t at = 0; at < count; ++at) {
                if (sample.at_us < output[at].start_us) {
                    phase = at ? between : before;
                    break;
                }
                if (sample.at_us < output[at].stop_us) {
                    phase = active;
                    break;
                }
                phase = after;
            }
            parts[phase].add(sample);
        }
        return true;
    }
};
} // namespace wifichat
