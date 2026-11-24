// ADR-0087 / C-BIND-004: same capability, two exact software endpoints and resource groups.
// Public C ABI scenarios; the normal-reopen case is explicitly authorized under ADR-0087.
// No power-cut injection or private Core state assertions.
#include "text_business_fixture.h"
#include "work_node.h"
#include <array>
#include <ayni/calls.h>

namespace {
using namespace ayni::test;
class BindingInstances : public ::testing::Test {
  protected:
    struct Endpoint {
        const char *id;
        uint32_t submitted = 0;
        uint64_t call = 0;
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                           ayni_provider_completion_t *completion) {
            auto &self = *static_cast<Endpoint *>(context);
            if (self.completion || string_of(request->endpoint_id) != self.id) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            ++self.submitted;
            self.call = request->logical_call_id;
            self.attempt = request->attempt;
            self.completion = completion;
            return AYNI_SUBMIT_ACCEPTED;
        }
        void finish() {
            if (!completion) {
                return;
            }
            ayni_provider_result_v1 result{};
            result.struct_size = sizeof(result);
            result.attempt = attempt;
            result.outcome = AYNI_CALL_COMPLETED;
            result.effect = AYNI_CALL_EFFECT_COMPLETE;
            result.result = text(id);
            ASSERT_EQ(ayni_provider_post(completion, &result), AYNI_OK);
            completion = nullptr;
        }
    };
    void start(const char *second_resource) {
        selected_b = second_resource;
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.call_capability_capacity = 1;
        config.provider_capacity = 2;
        config.binding_capacity = 2;
        config.resource_capacity = 2;
        config.resource_request_capacity = 4;
        config.call_input_max_bytes = 128;
        config.call_result_max_bytes = 64;
        config.storage_buffer_bytes = 0;
        config.goal_type_capacity = 4;
        work.additional_registration = [this](auto *kernel) { configure(kernel); };
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto status = snapshot.call_result(snapshot.call_context, text("echo"), &result);
            if (status == AYNI_OK) {
                const auto expected =
                    snapshot.owner_instance_id == instance_a ? first.id : second.id;
                if (result.outcome != AYNI_CALL_COMPLETED || string_of(result.text) != expected) {
                    return AYNI_INVALID_STATE;
                }
                return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
            }
            if (status != AYNI_NOT_FOUND) {
                return status;
            }
            TextValue input(text("hello"));
            ayni_call_request_v1 request{};
            request.struct_size = sizeof(request);
            request.kind = AYNI_POLICY_REQUEST_CALL;
            request.step_key = text("echo");
            request.binding_id =
                text(snapshot.owner_instance_id == instance_a ? "echo.a" : "echo.b");
            request.binding_version = 1;
            request.value = input.view();
            return output.request_call(output.call_context, &request);
        };
        work.run();
        instance_a = work.instance(agent_a);
        uint64_t command = 0;
        for (auto agent : {agent_a, agent_b}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             text("echo"),
                                             work.instance(agent),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = ++command;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        pump();
    }
    void configure(ayni_kernel_t *kernel) {
        const ayni_capability_desc_v1 authority{sizeof(authority), 1, text("echo"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        for (const auto name : {"slot.a", "slot.b"}) {
            const ayni_resource_desc_v1 resource{
                sizeof(resource), AYNI_RESOURCE_SHARED, text(name), 1, 0, 0, 0, nullptr, {}};
            ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
            ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                      AYNI_OK);
        }
        const ayni_call_resource_v1 demand{text("logical.slot"), 1, 0};
        TextBusiness business(64, 64);
        ayni_call_capability_v1 capability{};
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = authority.capability_id;
        capability.business = &business.definition;
        capability.contract = {sizeof(capability.contract),
                               AYNI_CALL_UNARY,
                               AYNI_CALL_TOOL,
                               0,
                               AYNI_CALL_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_NONE,
                               0,
                               30000,
                               AYNI_CALL_INPUT_VALUE,
                               AYNI_CALL_RESULT_TEXT,
                               88,
                               64,
                               1,
                               &demand,
                               nullptr,
                               AYNI_CALL_DURABILITY_DURABLE,
                               0};
        ASSERT_EQ(ayni_register_call_capability(kernel, &capability), AYNI_OK);
        for (auto *endpoint : {&first, &second}) {
            ayni_provider_v1 provider{};
            provider.struct_size = sizeof(provider);
            provider.version = 1;
            provider.provider_id = provider.endpoint_id = text(endpoint->id);
            provider.ctx = endpoint;
            provider.capability_count = 1;
            provider.capabilities = &capability;
            provider.submit = Endpoint::submit;
            ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
            // Registration copies these stack-local maps, including their ID bytes.
            char resource[] = "slot.a";
            if (endpoint == &second) {
                std::memcpy(resource, selected_b, sizeof(resource));
            }
            const ayni_binding_resource_v1 mapping{demand.resource_id, text(resource)};
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          text(endpoint == &first ? "echo.a" : "echo.b"),
                                          capability.capability_id,
                                          1,
                                          1,
                                          provider.provider_id,
                                          provider.endpoint_id,
                                          {},
                                          1,
                                          0,
                                          &mapping};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
            resource[0] = 'x';
        }
        auto type = goal_type_desc("echo", 1);
        type.export_requirements = [](void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 required{
                sizeof(required), 1, text("echo"), {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &required);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    }
    void pump() {
        constexpr uint32_t rounds = 80;
        for (uint32_t at = 0; at < rounds; ++at) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(1000);
            work.node().step();
        }
    }
    uint64_t create(uint64_t agent) {
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent), 0, "echo", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(created.status, AYNI_OK);
        pump();
        return created.result.object.id;
    }
    void TearDown() override {
        // Close every accepted delivery before any context can be destroyed.
        first.finish();
        second.finish();
        if (work.kernel()) {
            pump();
            second.finish(); // A shared waiter may have become dispatchable after first.finish.
            pump();
            ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
            work.node().drain();
            EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
        }
    }
    Endpoint first{"endpoint.a"}, second{"endpoint.b"};
    const char *selected_b = "slot.b";
    uint64_t instance_a = 0;
    WorkNode work; // Destroyed first, while producer contexts still exist.
};

TEST_F(BindingInstances, exact_routes_with_independent_resources_run_concurrently) {
    start("slot.b");
    const auto a = create(agent_a);
    const auto b = create(agent_b);
    ASSERT_EQ(first.submitted, 1U);
    ASSERT_EQ(second.submitted, 1U);
    EXPECT_NE(first.call, second.call);
    first.finish();
    second.finish();
    pump();
    EXPECT_EQ(work.task(work.goal(a).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(work.goal(b).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
}

TEST_F(BindingInstances, shared_resource_queues_second_instance_until_release) {
    start("slot.a");
    create(agent_a);
    const auto b = create(agent_b);
    ASSERT_EQ(first.submitted, 1U);
    EXPECT_EQ(second.submitted, 0U);
    auto call = ayni_call_info_v1{};
    call.struct_size = sizeof(call);
    const auto task = work.task(work.goal(b).last_task_id);
    ASSERT_EQ(ayni_get_call(work.kernel(), task.logical_call_id, &call), AYNI_OK);
    EXPECT_EQ(call.phase, AYNI_CALL_QUEUED);
    const auto accepted = call.logical_call_id;
    first.finish();
    pump();
    ASSERT_EQ(second.submitted, 1U);
    EXPECT_EQ(second.call, accepted); // Acceptance never selects the now-free first endpoint.
    second.finish();
    pump();
    EXPECT_EQ(work.task(task.task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// ADR-0087: user 2026-10-31 authorized only the package's minimal normal reopen evidence.
TEST_F(BindingInstances, orderly_reopen_retains_mapped_call_and_exact_result) {
    start("slot.b");
    create(agent_a);
    ASSERT_EQ(first.submitted, 1U);
    const auto original = first.call;
    first.finish();
    pump();
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    pump();
    auto info = ayni_call_info_v1{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_call(work.kernel(), original, &info), AYNI_OK);
    EXPECT_EQ(info.outcome, AYNI_CALL_COMPLETED);
    char result[64]{};
    uint32_t bytes = 0;
    ASSERT_EQ(ayni_copy_call_result(work.kernel(), original, result, sizeof(result), &bytes),
              AYNI_OK);
    EXPECT_EQ(std::string(result, bytes), first.id);
    EXPECT_EQ(first.submitted, 1U);
    EXPECT_EQ(second.submitted, 0U);
}
} // namespace
