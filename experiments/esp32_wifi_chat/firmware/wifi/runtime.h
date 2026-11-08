#pragma once
namespace wifichat {
class Transport;
}
namespace wifichat::wifi {
// Boot-lifetime composition. Never returns; all networking is on API workers,
// leaving this task responsive to touch and Core's owner independent of I/O.
void run(Transport &transport);
} // namespace wifichat::wifi
