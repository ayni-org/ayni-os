#pragma once
#include <string_view>
namespace wifichat {
enum class TurnStatus { progress, complete, failed };
// The owner/UI protocol has one terminal classifier. Legacy input errors are
// still terminal so a queued old diagnostic cannot leave the frontend busy.
inline TurnStatus turn_status(std::string_view text) {
    if (text == "reply completed") {
        return TurnStatus::complete;
    }
    if (text.find("turn failed") == 0 || text.find("input refused") == 0 ||
        text.find("input error=") == 0) {
        return TurnStatus::failed;
    }
    return TurnStatus::progress;
}
} // namespace wifichat
