#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
namespace wifichat::wifi {
// HTTP wire accounting only, never a buffer allocation. Model SSE has no total
// byte cap: its parser bounds retained fields/events, and the job bounds time.
struct ResponseLimit {
    std::optional<size_t> maximum_bytes; // Absent for model streams; present for JSON/audio.
    bool accepts(uint64_t bytes) const { return !maximum_bytes || bytes <= *maximum_bytes; }
    bool received(size_t &total, size_t bytes) const {
        if (bytes > std::numeric_limits<size_t>::max() - total) {
            return false;
        }
        // Count the observed chunk even if a bounded audio response exceeds its cap.
        total += bytes;
        return accepts(total);
    }
};
} // namespace wifichat::wifi
