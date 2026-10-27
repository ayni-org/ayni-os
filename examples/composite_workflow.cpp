// E4 normal subset: Task-owned capture -> model -> confirmed capture close -> notification.
#include "support.h"
#include <ayni/observations.h>
#include <cinttypes>
namespace {
using namespace ayni::examples;
struct Workflow {
    ayni::runtime::examples::Fixture *fixture = nullptr;
    static ayni_status_t requirements(void *ctx, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        const auto &f = *static_cast<Workflow *>(ctx)->fixture;
        for (const auto alias : {"observe", "summary", "notify"}) {
            const ayni_goal_requirement_v1 need{sizeof(need), 1, f.capability(alias),
                                                f.scope(alias)};
            const auto status = sink->emit(sink->ctx, &need);
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    static ayni_status_t decide(void *ctx, const ayni_task_snapshot_v1 *s,
                                ayni_policy_output_v1 *out) {
        auto &f = *static_cast<Workflow *>(ctx)->fixture;
        auto subscription = ayni::initialized<ayni_subscription_info_v1>();
        auto status = s->subscription(s->call_context, text("workflow.capture"), &subscription);
        if (status == AYNI_NOT_FOUND) {
            return request(*out, AYNI_POLICY_REQUEST_CALL, "workflow.capture", "observe");
        }
        if (status != AYNI_OK) {
            return status;
        }
        ayni_task_call_result_v1 analysis{};
        status = result(*s, "workflow.analysis", analysis);
        if (status == AYNI_NOT_FOUND) {
            auto sample = ayni::initialized<ayni_observation_info_v1>();
            status =
                s->subscription_observation(s->call_context, text("workflow.capture"), &sample);
            if (status == AYNI_NOT_FOUND || (status == AYNI_OK && !sample.usable)) {
                return request(*out, AYNI_POLICY_WAIT_SUBSCRIPTION, "workflow.capture");
            }
            if (status != AYNI_OK) {
                return status;
            }
            constexpr size_t prompt_capacity = 384;
            char prompt[prompt_capacity]{};
            const auto bytes =
                std::snprintf(prompt, prompt_capacity,
                              "Analyze this captured Mock scalar as historical inference, never "
                              "current action evidence: "
                              "source=%.*s boot=%" PRIu64 " generation=%" PRIu64
                              " sequence=%" PRIu64 " value=%" PRId64,
                              static_cast<int>(sample.reference.source_id.size),
                              sample.reference.source_id.data, sample.reference.boot_epoch,
                              sample.reference.generation, sample.reference.sequence, sample.value);
            if (bytes < 0 || static_cast<size_t>(bytes) >= prompt_capacity) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            return request(*out, AYNI_POLICY_REQUEST_MODEL, "workflow.analysis", "summary",
                           {prompt, static_cast<uint32_t>(bytes)}, f.scope("summary"));
        }
        if (status != AYNI_OK) {
            return status;
        }
        if (subscription.phase == AYNI_SUBSCRIPTION_ACTIVE ||
            subscription.phase == AYNI_SUBSCRIPTION_STARTING) {
            return request(*out, AYNI_POLICY_CLOSE_SUBSCRIPTION, "workflow.capture");
        }
        if (subscription.phase != AYNI_SUBSCRIPTION_STOPPED) {
            return request(*out, AYNI_POLICY_WAIT_SUBSCRIPTION, "workflow.capture");
        }
        ayni_task_call_result_v1 notification{};
        status = result(*s, "workflow.present", notification);
        if (status == AYNI_NOT_FOUND) {
            return request(*out, AYNI_POLICY_REQUEST_CALL, "workflow.present", "notify",
                           analysis.text);
        }
        if (status != AYNI_OK) {
            return status;
        }
        if (notification.effect != AYNI_CALL_EFFECT_COMPLETE) {
            return AYNI_INVALID_STATE;
        }
        return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
    }
    static ayni_status_t configure(void *ctx, ayni_kernel_t *kernel) {
        return install(kernel, "composite_workflow", "example.composite", ctx, decide,
                       requirements);
    }
};
} // namespace
int main() {
    Workflow workflow;
    ayni::runtime::examples::Fixture fixture;
    workflow.fixture = &fixture;
    auto status = fixture.open({&workflow, Workflow::configure});
    for (const auto alias : {"observe", "summary", "notify"}) {
        if (status == AYNI_OK) {
            status = fixture.grant(fixture.capability(alias), fixture.scope(alias));
        }
    }
    auto request = goal(fixture, "composite_workflow");
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(request, id);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    if (status == AYNI_OK && fixture.notifications() != 1) {
        status = AYNI_INVALID_STATE;
    }
    return finish("composite_workflow (normal E4 subset)", status, fixture);
}
