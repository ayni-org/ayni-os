// Event rules need no Core branch and no model; a copied local input wakes one Task.
#include "support.h"
namespace {
using namespace ayni::examples;
struct Rules {
    uint32_t decisions = 0;
    static ayni_status_t decide(void *ctx, const ayni_task_snapshot_v1 *s,
                                ayni_policy_output_v1 *out) {
        ++static_cast<Rules *>(ctx)->decisions;
        return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
    }
    static ayni_status_t configure(void *ctx, ayni_kernel_t *kernel) {
        return install(kernel, "example.event", "example.event_rules", ctx, decide, nullptr,
                       AYNI_TRIGGER_LOCAL_INPUT);
    }
};
} // namespace
int main() {
    Rules rules;
    ayni::runtime::examples::Fixture fixture;
    auto status = fixture.open({&rules, Rules::configure});
    auto request = goal(fixture, "example.event");
    request.trigger = AYNI_TRIGGER_LOCAL_INPUT;
    request.trigger_source = text("example.button.input");
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(request, id);
    }
    const ayni_event_v1 press{sizeof(press), AYNI_EVENT_LOCAL_INPUT, text("example.button.input")};
    if (status == AYNI_OK) {
        status = fixture.post(press);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    if (status == AYNI_OK && rules.decisions != 1) {
        status = AYNI_INVALID_STATE;
    }
    return finish("new_strategy (event rules)", status, fixture);
}
