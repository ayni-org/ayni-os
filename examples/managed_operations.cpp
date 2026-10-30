// ADR-0086: public SDK composition of a real software action and subscription.
#include "support.h"
#include <ayni/operations.h>
namespace {
using namespace ayni::examples;
struct Example {
    bool observed = false;
    static ayni_status_t requirements(void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        for (const auto id : {"software.sum", "software.events"}) {
            const ayni_goal_requirement_v1 requirement{sizeof(requirement), 1, text(id),
                                                       empty_scope()};
            const auto status = sink->emit(sink->ctx, &requirement);
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    static ayni_status_t start(ayni_policy_output_v1 &out, const char *key, const char *binding) {
        auto call = ayni::initialized<ayni_call_request_v1>();
        call.kind = AYNI_POLICY_REQUEST_CALL;
        call.step_key = text(key);
        call.binding_id = text(binding);
        call.binding_version = 1;
        call.mode = AYNI_CALL_MODE_MANAGED;
        ayni::ValueBuilder<1> input;
        if (equal(call.binding_id, "software.sum")) {
            input.integer(0, 512);
        } else {
            input.text(0, {});
        }
        const auto status = input.finish(call.value);
        return status == AYNI_OK ? out.request_call(out.call_context, &call) : status;
    }
    static ayni_status_t decide(void *context, const ayni_task_snapshot_v1 *snapshot,
                                ayni_policy_output_v1 *out) {
        auto &self = *static_cast<Example *>(context);
        auto job = ayni::initialized<ayni_task_call_result_v1>();
        auto stream = ayni::initialized<ayni_task_call_result_v1>();
        auto status = snapshot->call_result(snapshot->call_context, text("job"), &job);
        if (status == AYNI_NOT_FOUND) {
            return start(*out, "job", "software.sum");
        }
        if (status != AYNI_OK) {
            return status;
        }
        status = snapshot->call_result(snapshot->call_context, text("stream"), &stream);
        if (status == AYNI_NOT_FOUND) {
            return start(*out, "stream", "software.events");
        }
        if (status != AYNI_OK) {
            return status;
        }
        const auto &target = job.outcome ? stream : job;
        const ayni_operation_ref_v1 reference{snapshot->task_id, target.logical_call_id,
                                              target.operation_id};
        auto info = ayni::initialized<ayni_operation_info_v1>();
        status = snapshot->operation(snapshot->call_context, &reference, &info);
        if (status != AYNI_OK) {
            return status;
        }
        if (job.outcome && job.outcome != AYNI_CALL_COMPLETED) {
            return AYNI_INVALID_STATE;
        }
        if (stream.outcome) {
            return self.observed && stream.outcome == AYNI_CALL_CANCELLED
                       ? report(*snapshot, *out, AYNI_POLICY_REPORT_COMPLETION)
                       : AYNI_INVALID_STATE;
        }
        char key[AYNI_CATALOG_ID_MAX_BYTES];
        const auto size = std::snprintf(key, sizeof(key), "control.%u", snapshot->revision);
        if (size <= 0 || static_cast<size_t>(size) >= sizeof(key)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto control = ayni::initialized<ayni_operation_control_v1>();
        control.step_key = {key, static_cast<uint32_t>(size)};
        control.reference = reference;
        if (job.outcome && info.sample.usable) {
            self.observed = true;
            control.kind = AYNI_OPERATION_CLOSE;
        } else {
            control.kind = AYNI_OPERATION_WAIT;
            control.after = info.frontier;
            control.timeout_ms = AYNI_OPERATION_WAIT_MS_MAX;
        }
        return out->control_operation(out->call_context, &control);
    }
    static ayni_status_t configure(void *context, ayni_kernel_t *kernel) {
        return install(kernel, "example.managed", "example.operations", context, decide,
                       requirements);
    }
};
} // namespace
int main() {
    Example example;
    ayni::runtime::examples::Fixture fixture;
    auto status = fixture.open({&example, Example::configure}, ayni::runtime::examples::Model::mock,
                               ayni::runtime::examples::Software::work);
    for (const auto id : {"software.sum", "software.events"}) {
        if (status == AYNI_OK) {
            status = fixture.grant(ayni::examples::text(id), ayni::examples::empty_scope());
        }
    }
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(ayni::examples::goal(fixture, "example.managed"), id);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    return ayni::examples::finish("managed_operations (sum and scalar stream)", status, fixture);
}
