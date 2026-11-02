// New model endpoint: the same public policy works with a second HTTP Provider.
#include "support.h"
namespace {
using namespace ayni::examples;
struct Policy {
    ayni::runtime::examples::Fixture *fixture = nullptr;
    static ayni_status_t requirements(void *ctx, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        auto &f = *static_cast<Policy *>(ctx)->fixture;
        const ayni_goal_requirement_v1 need{sizeof(need), 1, f.capability("alternate"),
                                            f.scope("alternate")};
        return sink->emit(sink->ctx, &need);
    }
    static ayni_status_t decide(void *ctx, const ayni_task_snapshot_v1 *s,
                                ayni_policy_output_v1 *out) {
        auto &f = *static_cast<Policy *>(ctx)->fixture;
        ayni_task_call_result_v1 answer{};
        const auto status = result(*s, "second.answer", answer);
        if (status == AYNI_NOT_FOUND) {
            return request(*out, AYNI_POLICY_REQUEST_MODEL, "second.answer", "alternate",
                           text("Say alternate fixture reply"), f.scope("alternate"));
        }
        if (status != AYNI_OK) {
            return status;
        }
        std::string reply;
        const auto read = ayni::examples::answer(f, answer, reply);
        if (read != AYNI_OK) {
            return read;
        }
        if (reply != "alternate fixture reply") {
            return AYNI_INVALID_STATE;
        }
        return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
    }
    static ayni_status_t configure(void *ctx, ayni_kernel_t *kernel) {
        return install(kernel, "example.model", "example.second_model", ctx, decide, requirements);
    }
};
} // namespace
int main() {
    Policy policy;
    ayni::runtime::examples::Fixture fixture;
    policy.fixture = &fixture;
    auto status = fixture.open({&policy, Policy::configure},
                               ayni::runtime::examples::Model::alternate_loopback);
    if (status == AYNI_OK) {
        status = fixture.grant(fixture.capability("alternate"), fixture.scope("alternate"));
    }
    auto request = goal(fixture, "example.model");
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(request, id);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    if (status == AYNI_OK && fixture.model_requests() != 1) {
        status = AYNI_INVALID_STATE;
    }
    return finish("new_model (loopback HTTP, no external API)", status, fixture);
}
