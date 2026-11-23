#include "model_delivery.h"
#include "../main/transport.h"
#include <esp_timer.h>
#include <freertos/task.h>

namespace wifichat::wifi {
void deliver_model_completion(Transport &transport, ModelCompletion result) {
    if (!result.ok && result.packet.kind != Kind::model_error) {
        // A network failure may have no classified terminal. Wake the owner with
        // the same empty RESULT_UNKNOWN packet as the ordinary backend path.
        result.packet.kind = Kind::model_error;
        result.packet.size = 0;
    }
    for (;;) {
        switch (transport.post_model(result.packet, result.timing)) {
        case ModelPost::accepted:
            if (result.ok && result.outline.size &&
                result.timing.generation == transport.api_generation()) {
                transport.post(Kind::tool_outline, result.packet.id, result.outline.bytes.data(),
                               result.outline.size);
            }
            return;
        case ModelPost::stale:
            return;
        case ModelPost::invalid:
            transport.diagnostics().record("wifi model_delivery invalid call=%llu\n",
                                           static_cast<unsigned long long>(result.packet.id));
            return;
        case ModelPost::busy:
            if (esp_timer_get_time() >= result.deadline_us) {
                return;
            }
            vTaskDelay(1);
            break;
        }
    }
}
} // namespace wifichat::wifi
