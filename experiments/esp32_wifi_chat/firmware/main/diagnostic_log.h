#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <ayni/values_json.hpp>
#include <bitset>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wifichat {
// Explicit metadata producers only: never pass model/user text, keys, arguments or media.
// Boot-lifetime RAM belongs to Transport. Slow readers overwrite no producer state;
// contended producers drop diagnostics rather than delaying Core or motor control.
class DiagnosticLog {
  public:
    static constexpr size_t slots = 128, line_bytes = 384, batch = 12;
    using Clock = uint64_t (*)();
    void initialize(uint32_t boot, Clock clock) {
        boot_ = boot;
        clock_ = clock;
    }
    // Producers only capture RAM. The composition logger alone performs serial I/O.
    void record(const char *format, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, format);
        write(true, format, args);
        va_end(args);
    }
    // Bounded formatting and RAM capture for additional owner timing metadata.
    void record_ram(const char *format, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, format);
        write(false, format, args);
        va_end(args);
    }
    // Retain one RAM event now and emit its exact serial copy outside the measured window.
    void record_deferred(const char *format, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, format);
        write(true, format, args);
        va_end(args);
    }
    template <class Sink> void drain_serial(Sink sink) {
        std::array<char, line_bytes> line;
        for (size_t count = 0; count < batch && take_serial(line); ++count) {
            sink(line.data());
        }
    }
    // Control workers only copy a bounded preformatted diagnostic; no UART or network I/O.
    void capture(std::string_view text) { capture_line(text, false); }
    // Called by the HTTP task with its own response lane. No socket I/O holds this lock.
    // Cursor is a decimal string on the wire, so JavaScript never rounds uint64 identities.
    bool json(ayni::json::Writer &out, uint64_t after) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        const auto oldest = oldest_sequence();
        const bool reset = after > sequence_;
        const auto start = reset ? oldest : std::max(after, oldest);
        const auto through = start + std::min<uint64_t>(sequence_ - start, batch);
        out.raw("{\"boot\":");
        out.unsigned_number(boot_);
        out.raw(",\"next\":\"");
        out.unsigned_number(through);
        out.raw("\",\"reset\":");
        out.raw(reset ? "true" : "false");
        out.raw(",\"lost\":");
        out.unsigned_number(!reset && after < oldest ? oldest - after : 0);
        out.raw(",\"dropped\":");
        out.unsigned_number(dropped_.load(std::memory_order_relaxed));
        out.raw(",\"events\":[");
        for (auto sequence = start; sequence < through; ++sequence) {
            const auto &entry = entries_[sequence % slots];
            if (sequence != start) {
                out.raw(",");
            }
            out.raw("{\"ms\":");
            out.unsigned_number(entry.ms);
            out.raw(",\"text\":");
            out.string(std::string_view(entry.text.data()));
            out.raw("}");
        }
        out.raw("]}");
        return true;
    }

  private:
    uint64_t oldest_sequence() const { return sequence_ > slots ? sequence_ - slots : 0; }
    void capture_line(std::string_view text, bool serial_pending) {
        if (text.empty()) {
            return;
        }
        if (text.size() >= line_bytes) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (sequence_ == UINT64_MAX) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto index = sequence_++ % slots;
        // Reusing a ring slot also retires its serial copy. HTTP accounts for
        // overwritten RAM entries through its own cursor.
        serial_pending_[index] = serial_pending;
        auto &entry = entries_[index];
        entry.ms = clock_ ? clock_() : 0;
        std::memcpy(entry.text.data(), text.data(), text.size());
        entry.text[text.size()] = 0;
    }
    // Copy one line under the ring lock. Serial I/O runs after releasing the lock.
    bool take_serial(std::array<char, line_bytes> &line) {
        std::lock_guard<std::mutex> lock(mutex_);
        serial_cursor_ = std::max(serial_cursor_, oldest_sequence());
        while (serial_cursor_ < sequence_) {
            const auto index = serial_cursor_++ % slots;
            if (serial_pending_[index]) {
                line = entries_[index].text;
                serial_pending_.reset(index);
                return true;
            }
        }
        return false;
    }

    void write(bool deferred, const char *format, va_list args) {
        char text[line_bytes]{};
        const auto size = std::vsnprintf(text, sizeof(text), format, args);
        if (size <= 0 || size_t(size) >= sizeof(text)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        capture_line({text, size_t(size)}, deferred);
    }
    struct Entry {
        uint64_t ms = 0;
        std::array<char, line_bytes> text{};
    };
    std::mutex mutex_;
    std::array<Entry, slots> entries_{};
    std::bitset<slots> serial_pending_{}; // Undrained copies of still-retained RAM entries.
    uint64_t sequence_ = 0;
    uint64_t serial_cursor_ = 0;
    std::atomic<uint32_t> dropped_{0};
    uint32_t boot_ = 0;
    Clock clock_ = nullptr;
};
} // namespace wifichat
