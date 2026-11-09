#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace wifichat::wifi {
// Exact independent experiment layout; never a label-only erase or whole-chip reset.
struct ResetPlan {
    static constexpr uint32_t address = 0x800000, bytes = 0x800000, chunk = 0x10000;
    static bool matches(std::string_view label, uint32_t offset, uint32_t size) {
        return label == "ayni" && offset == address && size == bytes;
    }
};
} // namespace wifichat::wifi
