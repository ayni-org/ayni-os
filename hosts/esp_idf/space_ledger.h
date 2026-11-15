// Conservative LittleFS space accounting; the exclusive volume owner observes all block writes.
#ifndef AYNI_ESP_SPACE_LEDGER_H
#define AYNI_ESP_SPACE_LEDGER_H
#include <array>
#include <cstdint>
#include <optional>
namespace ayni::hosts::esp_idf {
class SpaceLedger {
  public:
    static constexpr uint32_t block_bytes = 4096;
    static constexpr uint32_t tracked_blocks = 64;
    static constexpr uint64_t reconciliation_bytes = 64U * 1024U;
    // A successful traversal supplies a new lower bound, already excluding metadata reserve.
    void measured(uint64_t free_bytes) { budget_.emplace(Budget{free_bytes}); }
    void invalidate() { budget_.reset(); }
    // Charge before program/erase, including errors. Counting already allocated blocks only
    // understates space; duplicate writes to one physical block do not consume another block.
    void changing(uint32_t block) {
        if (!budget_) {
            return;
        }
        for (uint32_t at = 0; at < budget_->count; ++at) {
            if (budget_->blocks[at] == block) {
                return;
            }
        }
        if (budget_->count == tracked_blocks) {
            invalidate();
            return;
        }
        budget_->blocks[budget_->count++] = block;
    }
    // False requests a real traversal; zero bytes remains a valid full-volume observation.
    bool available(uint64_t &bytes) const {
        if (!budget_) {
            return false;
        }
        const uint64_t debit = uint64_t(budget_->count) * block_bytes;
        bytes = budget_->free_bytes > debit ? budget_->free_bytes - debit : 0;
        return true;
    }
    // Reconcile bounded accumulated debt or a low-space mutation. Unchanged full volumes
    // retain a valid zero estimate, rather than rescanning on every idle tick.
    bool needs_measurement() const {
        uint64_t bytes = 0;
        return !available(bytes) ||
               (budget_->count != 0 &&
                (bytes < reconciliation_bytes ||
                 uint64_t(budget_->count) * block_bytes >= reconciliation_bytes));
    }

  private:
    struct Budget {
        uint64_t free_bytes; // Measured usable lower bound; reclaim is not credited speculatively.
        std::array<uint32_t, tracked_blocks> blocks{};
        uint32_t count = 0; // Initialized prefix of blocks, each distinct since measurement.
    };
    std::optional<Budget> budget_; // None until measured, or after tracking capacity is exhausted.
};
} // namespace ayni::hosts::esp_idf
#endif
