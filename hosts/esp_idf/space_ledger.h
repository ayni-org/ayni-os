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
    enum class Reconciliation { clean, routine, urgent };

    // The Host supplies other accepted reservations. Without fresh blocks to reconcile, a
    // current zero lower bound stays valid and does not cause repeated traversals.
    Reconciliation reconciliation(uint64_t reserved = 0) const {
        uint64_t bytes = 0;
        if (!available(bytes)) {
            return Reconciliation::urgent;
        }
        if (!budget_->count) {
            return Reconciliation::clean;
        }
        if (budget_->count == tracked_blocks || bytes <= reserved ||
            bytes - reserved < reconciliation_bytes) {
            return Reconciliation::urgent;
        }
        return uint64_t(budget_->count) * block_bytes >= reconciliation_bytes
                   ? Reconciliation::routine
                   : Reconciliation::clean;
    }

  private:
    struct Budget {
        uint64_t free_bytes; // Measured usable lower bound; reclaim is not credited speculatively.
        std::array<uint32_t, tracked_blocks> blocks{};
        uint32_t count = 0; // Initialized prefix of blocks, each distinct since measurement.
    };
    std::optional<Budget> budget_; // None until measured, or after tracking capacity is exhausted.
};
// One worker-owned deadline, measured from the first service boundary with routine debt.
class SpaceRefresh {
  public:
    static constexpr uint64_t max_deferral_us = 1000000;

    bool measure(SpaceLedger::Reconciliation need, uint64_t now_us, bool quiet) {
        using Need = SpaceLedger::Reconciliation;
        if (need != Need::routine) {
            due_since_.reset();
            return need == Need::urgent;
        }
        if (!due_since_) {
            due_since_ = now_us;
        }
        if (!quiet && now_us - *due_since_ < max_deferral_us) {
            return false;
        }
        due_since_.reset();
        return true;
    }
    bool pending() const { return due_since_.has_value(); }

  private:
    std::optional<uint64_t> due_since_;
};
} // namespace ayni::hosts::esp_idf
#endif
