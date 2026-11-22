#pragma once
#include "../main/model_handoff.h"
#include "../main/tool_outline.h"

namespace wifichat {
class Transport;
namespace wifi {
// Worker-owned completion. Delivery retains the packet only while the dedicated slot is busy.
struct ModelCompletion {
    Packet &packet;
    const ToolOutline &outline;
    ModelDeliveryTiming timing;
    int64_t deadline_us;
    bool ok;
};
void deliver_model_completion(Transport &, ModelCompletion);
} // namespace wifi
} // namespace wifichat
