#pragma once
#include <cstdint>
#include <string_view>
namespace wifichat::wifi {
// Reject a cold-boot clock before HTTPS. TLS still verifies certificate dates;
// this threshold is a readiness check, not an alternative trust or time source.
inline constexpr int64_t minimum_network_epoch_seconds = 1700000000;
inline bool network_clock_ready(int64_t epoch_seconds) {
    return epoch_seconds >= minimum_network_epoch_seconds;
}
inline bool request_clock_ready(std::string_view url, int64_t epoch_seconds) {
    constexpr std::string_view https_scheme = "https://";
    return url.substr(0, https_scheme.size()) != https_scheme || network_clock_ready(epoch_seconds);
}
} // namespace wifichat::wifi
