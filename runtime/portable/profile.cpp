#include <algorithm>
#include <array>
#include <ayni/applications/software_workflow.h>
#include <ayni/host_authority.h>
#include <ayni/providers/fixture_model.h>
#include <ayni/providers/mock_execution.h>
#include <ayni/providers/software/work.h>
#include <ayni/resources.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/runtime/common/harness_settings.h>
#include <ayni/runtime/common/memory_capability.h>
#include <ayni/runtime/portable.h>
#include <ayni/scenes.h>
#include <cstring>
#include <new>
namespace ayni::runtime::portable {
namespace {
constexpr uint32_t version = 1;
constexpr uint32_t step_units = 64, step_us = 5000;
constexpr uint32_t extra_goal_types = 6, extra_strategies = 4;
constexpr uint32_t retained_calls = 32, retained_steps = 64;
constexpr uint32_t source_capacity = 4, resource_capacity = 8, catalog_capacity = 8;
constexpr uint32_t kernel_command_capacity = 4;
constexpr uint32_t ingress_capacity = 8, authority_scope_bytes = 256, retained_goals = 8;
constexpr uint32_t memory_capacity = 8;
constexpr uint32_t write_interval_us = 1000, journal_bytes = 512 * 1024;
// Fixture models: small window so the normal suite crosses compaction (ADR-0095).
constexpr runtime::HarnessSettings harness{12288, 3072, 4096, 2304, 8, 30000, 1};
constexpr int64_t execution_minimum = -100, execution_maximum = 100;
constexpr uint32_t execution_lease_us = 200000, command_age_us = 100000;
constexpr uint64_t command_range = UINT64_C(1) << 32;
constexpr uint64_t event_interval_us = 10000;
constexpr const char *chat_types[agent_count] = {"portable.chat.a", "portable.chat.b"};
constexpr const char *model_bindings[agent_count] = {"portable.model.a", "portable.model.b"};
constexpr const char *model_resources[agent_count] = {"model.a", "model.b"};
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
ayni_scope_v1 scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
// The one yaw limit field; the clock is absent only when sizing the Kernel.
providers::mock_execution::Config execution_config(const ayni_host_v1 *host) {
    providers::mock_execution::Config config;
    if (host) {
        config.clock_context = host->ctx;
        config.now_us = host->monotonic_now_us;
        config.executor_boot_epoch = host->monotonic_now_us(host->ctx) + 1;
    }
    config.limits.count = 1;
    auto &limit = config.limits.fields[0];
    std::memcpy(limit.field_id.data, "yaw", sizeof("yaw") - 1);
    limit.field_id.size = sizeof("yaw") - 1;
    std::memcpy(limit.unit.data, "mrad", sizeof("mrad") - 1);
    limit.unit.size = sizeof("mrad") - 1;
    limit.minimum = execution_minimum;
    limit.maximum = execution_maximum;
    return config;
}
// One deterministic region planner for sizing and placement; queries never create objects.
struct Regions {
    uint8_t *base;
    size_t used = 0, alignment = 1;
    void *take(size_t bytes, size_t align) {
        alignment = std::max(alignment, align);
        used = (used + align - 1) & ~(align - 1);
        void *out = base ? base + used : nullptr;
        used += bytes;
        return out;
    }
};
} // namespace
ayni_config_v1 configuration() {
    auto c = default_config();
    c.ingress_capacity = ingress_capacity;
    c.authority_scope_max_bytes = authority_scope_bytes;
    c.agent_capacity = agent_count;
    c.goal_capacity =
        retained_goals; // Retains normal terminal evidence separately from the four Task slots.
    c.task_capacity = live_task_limit;
    c.goal_type_capacity = agent_count + extra_goal_types;
    c.strategy_capacity = agent_count + extra_strategies;
    c.goal_parameter_max_bytes = strategies::ToolLoop::checkpoint_bytes;
    c.task_checkpoint_max_bytes = strategies::ToolLoop::checkpoint_bytes;
    c.task_step_capacity = retained_steps;
    c.call_capacity = retained_calls;
    c.max_turns = harness.max_turns;
    fit_call_bytes(c, {{providers::fixture_model::call_input_bytes(true),
                        providers::fixture_model::call_result_bytes},
                       {providers::software::call_bytes, providers::software::call_bytes},
                       {providers::mock_execution::call_input_bytes(execution_config(nullptr)),
                        providers::mock_execution::call_result_bytes}});
    c.capability_capacity = catalog_capacity;
    c.call_capability_capacity = catalog_capacity;
    c.provider_capacity = catalog_capacity;
    c.binding_capacity = catalog_capacity;
    c.resource_capacity = resource_capacity;
    c.resource_request_capacity = live_task_limit;
    c.observation_source_capacity = source_capacity;
    c.scene_capacity = 1;
    c.executor_capacity = 1;
    c.execution_binding_capacity = 1;
    c.memory_capacity = memory_capacity;
    c.command_capacity = kernel_command_capacity;
    c.storage_write_interval_us = write_interval_us;
    c.storage_buffer_bytes = journal_bytes;
    return c;
}
struct Profile::Impl {
    explicit Impl(const ayni_host_v1 &ports, hosts::storage::FileValuePort files)
        : host(ports), values(files),
          model_a(providers::fixture_model::Variant::a, providers::fixture_model::Service::tools,
                  host.ctx, host.monotonic_now_us, bodies.store()),
          model_b(providers::fixture_model::Variant::b, providers::fixture_model::Service::tools,
                  host.ctx, host.monotonic_now_us, bodies.store()),
          summary(providers::fixture_model::Variant::a, providers::fixture_model::Service::summary,
                  host.ctx, host.monotonic_now_us, bodies.store()) {}
    ayni_host_v1 host;
    hosts::storage::FileValuePort values;
    runtime::FileBodies bodies; // Every conversation text and model body, on the value files.
    ayni_kernel_t *kernel = nullptr;
    providers::fixture_model::Provider model_a, model_b, summary;
    providers::software::WorkProvider software;
    providers::mock_execution::Executor executor;
    std::array<strategies::ToolLoop, agent_count> loops;
    // One logical owner invokes these synchronous consumers without overlap.
    // No scratch pointer survives a decide/codec callback; worker I/O uses separate file lanes.
    ayni_workspace_v1 owner_region{};
    std::array<runtime::conversation::Session, agent_count> conversations;
    std::array<bool, agent_count>
        conversation_bound{}; // Startup bindings only, never domain state.
    uint64_t session = 0, boot = 0, command_sequence = 0;
    std::optional<uint64_t> pending_command;
    std::array<uint64_t, agent_count> instances{}; // Public stable IDs, initialized once per boot.

    ayni_status_t receipts() {
        if (!pending_command) {
            return AYNI_OK;
        }
        auto receipt = initialized<ayni_intent_receipt_v1>();
        const auto status =
            ayni_get_intent_receipt(kernel, owner_principal, *pending_command, &receipt);
        if (status == AYNI_OK && receipt.phase == AYNI_INTENT_DURABLE) {
            pending_command.reset();
        }
        return status;
    }
    static Submission command(void *context, ayni_intent_kind_t kind, const void *body,
                              uint32_t revision) {
        auto &self = *static_cast<Impl *>(context);
        Submission out;
        out.status = self.receipts();
        if (out.status != AYNI_OK) {
            return out;
        }
        if (self.pending_command) {
            out.status = AYNI_BUSY;
            return out;
        }
        if (!self.session || !self.boot || self.boot >= UINT64_MAX / command_range ||
            self.command_sequence == command_range - 1) {
            out.status = AYNI_NOT_READY;
            return out;
        }
        const ayni_intent_v1 intent{sizeof(intent),
                                    kind,
                                    self.boot * command_range + ++self.command_sequence,
                                    self.session,
                                    revision,
                                    0,
                                    body};
        out.status = ayni_submit_intent(self.kernel, &intent, &out.result, &out.error);
        if (out.status == AYNI_OK && out.result.phase != AYNI_INTENT_DURABLE) {
            self.pending_command = intent.command_id;
        }
        return out;
    }
    ayni_status_t resource(const char *id, uint32_t capacity) {
        const ayni_resource_desc_v1 desc{sizeof(desc),
                                         capacity == 1 ? AYNI_RESOURCE_EXCLUSIVE
                                                       : AYNI_RESOURCE_SHARED,
                                         text(id),
                                         capacity,
                                         0,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
        const auto status = ayni_register_resource(kernel, &desc);
        return status == AYNI_OK
                   ? ayni_set_resource_health(kernel, desc.resource_id, AYNI_RESOURCE_ONLINE)
                   : status;
    }
    // Distinct Provider instances share an exact capability. Only Binding maps
    // its logical demand, without changing authority/business/execution semantics.
    ayni_status_t offer(const ayni_provider_v1 &provider, bool new_capability) {
        for (uint32_t at = 0; new_capability && at < provider.capability_count; ++at) {
            const auto &call = provider.capabilities[at];
            const ayni_capability_desc_v1 cap{sizeof(cap), version, call.capability_id,
                                              0,           0,       nullptr};
            auto status = ayni_register_capability(kernel, &cap);
            if (status == AYNI_OK) {
                status = ayni_register_call_capability(kernel, &call);
            }
            if (status != AYNI_OK) {
                return status;
            }
        }
        return ayni_register_provider(kernel, &provider);
    }
    ayni_status_t binding(const ayni_provider_v1 &p, uint32_t at, const char *id,
                          const char *actual = nullptr) {
        const ayni_binding_resource_v1 mapping{text("model.slot"),
                                               text(actual ? actual : "model.slot")};
        const ayni_binding_v1 b{sizeof(b),
                                version,
                                text(id),
                                p.capabilities[at].capability_id,
                                version,
                                version,
                                p.provider_id,
                                p.endpoint_id,
                                {},
                                actual ? 1U : 0U,
                                0,
                                actual ? &mapping : nullptr};
        return ayni_register_binding(kernel, &b);
    }
    ayni_status_t models() {
        std::array<providers::fixture_model::Provider *, agent_count + 1> models{&model_a, &model_b,
                                                                                 &summary};
        for (uint32_t at = 0; at < models.size(); ++at) {
            ayni_provider_v1 provider{};
            auto status = models[at]->describe(provider);
            if (status == AYNI_OK) {
                status = offer(provider, at != 1);
            }
            if (status == AYNI_OK) {
                status =
                    binding(provider, 0, at < agent_count ? model_bindings[at] : "portable.summary",
                            at < agent_count ? model_resources[at] : nullptr);
            }
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    ayni_status_t software_catalog() {
        providers::software::WorkConfig config;
        config.clock_context = host.ctx;
        config.now_us = host.monotonic_now_us;
        config.event_interval_us = event_interval_us;
        auto status = software.configure(config);
        if (status == AYNI_OK) {
            status = resource("software.slot", config.capacity);
        }
        ayni_observation_source_v1 source{};
        if (status == AYNI_OK) {
            status = software.describe_source(source);
        }
        if (status == AYNI_OK) {
            status = ayni_register_observation_source(kernel, &source);
        }
        ayni_provider_v1 provider{};
        if (status == AYNI_OK) {
            status = software.describe(provider);
        }
        if (status == AYNI_OK) {
            status = offer(provider, true);
        }
        for (uint32_t at = 0; status == AYNI_OK && at < provider.capability_count; ++at) {
            const auto id = provider.capabilities[at].capability_id;
            const ayni_binding_v1 b{sizeof(b),
                                    version,
                                    id,
                                    id,
                                    version,
                                    version,
                                    provider.provider_id,
                                    provider.endpoint_id,
                                    {},
                                    0,
                                    0,
                                    nullptr};
            status = ayni_register_binding(kernel, &b);
            ayni_tool_usage_v1 usage{};
            if (status == AYNI_OK) {
                status = software.usage(id, {id, version, 0}, usage);
            }
            if (status == AYNI_OK) {
                status = ayni_register_capability_usage(kernel, usage.purpose, &usage);
            }
        }
        return status;
    }
    // Explicit simulated execution profile. It owns no driver or real actuator;
    // Core still requires a durable Scene, Grant and independently checked permit.
    ayni_status_t execution_catalog() {
        const auto config = execution_config(&host);
        auto status = executor.configure(config);
        ayni_provider_v1 provider{};
        if (status == AYNI_OK) {
            status = executor.describe_provider(provider);
        }
        const ayni_authority_value_set_v1 domain{sizeof(domain),
                                                 AYNI_AUTHORITY_I64,
                                                 text("mrad"),
                                                 {{execution_minimum, execution_maximum}}};
        const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                 domain};
        const ayni_capability_desc_v1 cap{sizeof(cap), version, text("mock.turn"), 1, 0, &field};
        if (status == AYNI_OK) {
            status = ayni_register_capability(kernel, &cap);
        }
        if (status == AYNI_OK) {
            status = ayni_register_call_capability(kernel, provider.capabilities);
        }
        if (status == AYNI_OK) {
            status = ayni_register_provider(kernel, &provider);
        }
        const ayni_resource_desc_v1 actuator{sizeof(actuator),
                                             AYNI_RESOURCE_EXCLUSIVE,
                                             text("mock.actuator"),
                                             1,
                                             AYNI_RESOURCE_ACTUATOR,
                                             0,
                                             0,
                                             nullptr,
                                             {}};
        if (status == AYNI_OK) {
            status = ayni_register_resource(kernel, &actuator);
        }
        if (status == AYNI_OK) {
            status = ayni_set_resource_health(kernel, actuator.resource_id, AYNI_RESOURCE_ONLINE);
        }
        if (status == AYNI_OK) {
            status = binding(provider, 0, "portable.turn");
        }
        const ayni_scene_rule_v1 rule{cap.capability_id, version, 0, scope()};
        const ayni_scene_desc_v1 scene{sizeof(scene), version, text("portable.scene"), 1, 0,
                                       &rule,         nullptr};
        if (status == AYNI_OK) {
            status = ayni_register_scene(kernel, &scene);
        }
        ayni_executor_v1 ports{};
        if (status == AYNI_OK) {
            status = executor.describe_executor(ports);
        }
        if (status == AYNI_OK) {
            status = ayni_register_executor(kernel, &ports);
        }
        const ayni_execution_field_v1 mapping{text("yaw"), text("mrad"), 1, 1};
        const ayni_execution_binding_v1 execution{sizeof(execution),
                                                  version,
                                                  text("portable.turn"),
                                                  ports.executor_id,
                                                  version,
                                                  version,
                                                  scene.scene_id,
                                                  AYNI_FORM_PROCEDURE,
                                                  execution_lease_us,
                                                  command_age_us,
                                                  1,
                                                  &mapping};
        if (status == AYNI_OK) {
            status = ayni_register_execution_binding(kernel, &execution);
        }
        return status;
    }
    ayni_status_t memory_catalog() {
        const auto fields = memory_access_fields();
        const ayni_capability_desc_v1 cap{
            sizeof(cap), version,      text("memory.access"), AYNI_MEMORY_SCOPE_FIELD_COUNT,
            0,           fields.data()};
        return ayni_register_capability(kernel, &cap);
    }
    static ayni_status_t requirements(void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        for (const auto *id : {"fixture.tools", "fixture.summary"}) {
            const ayni_goal_requirement_v1 req{sizeof(req), version, text(id), scope()};
            const auto status = sink->emit(sink->ctx, &req);
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    ayni_status_t policies() {
        for (uint32_t at = 0; at < agent_count; ++at) {
            strategies::ToolLoopConfig c;
            c.identity.id = text(chat_types[at]);
            c.limits = {harness.context_window_tokens, harness.max_output_tokens,
                        harness.compact_target_tokens, harness.tool_result_max_tokens};
            c.bodies = bodies.store();
            c.history = {&bodies, runtime::FileBodies::ready, nullptr, nullptr};
            c.model.kind = c.summary.kind = AYNI_POLICY_REQUEST_MODEL;
            c.model.binding_id = text(model_bindings[at]);
            c.summary.binding_id = text("portable.summary");
            auto status = loops[at].configure(c, &owner_region);
            const auto strategy = loops[at].describe();
            if (status == AYNI_OK) {
                status = ayni_register_strategy(kernel, &strategy);
            }
            const ayni_tool_ref_v1 tool{text("software.sum"), version, AYNI_TOOL_ROLE_CAPABILITY};
            auto type = initialized<ayni_goal_type_v1>();
            type.version = version;
            type.goal_type_id = text(chat_types[at]);
            type.ctx = &owner_region;
            type.validate = strategies::Conversation::validate_turn;
            type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
            type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
            type.strategy_id = strategy.strategy_id;
            type.export_requirements = requirements;
            type.tool_count = 1;
            type.tool_authorization = AYNI_TOOLS_ON_DEMAND;
            type.tools = &tool;
            type.context_mode = AYNI_GOAL_CONTEXT_AGENT;
            if (status == AYNI_OK) {
                status = ayni_register_goal_type(kernel, &type);
            }
            if (status != AYNI_OK) {
                return status;
            }
        }
        return applications::software_workflow::configure(&owner_region, kernel);
    }
    ayni_status_t configure() {
        const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, owner_principal,
                                           version, 0};
        auto status = ayni_register_principal(kernel, &owner);
        for (uint64_t id = 1; status == AYNI_OK && id <= agent_count; ++id) {
            const ayni_agent_desc_v1 agent{sizeof(agent), 0, id};
            status = ayni_register_agent(kernel, &agent);
        }
        for (const auto *id : {"model.slot", "model.a", "model.b", "summary.slot"}) {
            if (status == AYNI_OK) {
                status = resource(id, 1);
            }
        }
        if (status == AYNI_OK) {
            status = models();
        }
        if (status == AYNI_OK) {
            status = software_catalog();
        }
        if (status == AYNI_OK) {
            status = execution_catalog();
        }
        if (status == AYNI_OK) {
            status = memory_catalog();
        }
        return status == AYNI_OK ? policies() : status;
    }
};
// Query every region with the component's public query. Placement uses this same
// order and alignment; no duplicated platform-specific arena size assumptions.
static ayni_status_t owner_workspace(Regions &regions, ayni_workspace_v1 *out) {
    size_t largest = 0, strictest_alignment = 1;
    for (const auto query :
         {strategies::ToolLoop::workspace_requirements,
          strategies::Conversation::workspace_requirements, ayni_value_workspace_requirements}) {
        size_t bytes = 0, alignment = 0;
        const auto status = query(&bytes, &alignment);
        if (status != AYNI_OK) {
            return status;
        }
        largest = std::max(largest, bytes);
        strictest_alignment = std::max(strictest_alignment, alignment);
    }
    void *data = regions.take(largest, strictest_alignment);
    if (out) {
        *out = {sizeof(*out), 0, data, largest};
    }
    return AYNI_OK;
}
ayni_status_t Profile::memory_requirements(Memory &out) {
    auto config = configuration();
    size_t bytes = 0, alignment = 0;
    auto status = ayni_kernel_memory_requirements(&config, &bytes, &alignment);
    if (status != AYNI_OK) {
        return status;
    }
    Regions regions{nullptr};
    regions.take(sizeof(Impl), alignof(Impl));
    regions.take(bytes, alignment);
    status = owner_workspace(regions, nullptr);
    if (status == AYNI_OK) {
        out = {regions.used, regions.alignment, bytes};
    }
    return status;
}
ayni_status_t Profile::create_in(void *data, size_t bytes, const ayni_host_v1 &host,
                                 hosts::storage::FileValuePort files) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    Memory required;
    auto status = memory_requirements(required);
    if (status != AYNI_OK) {
        return status;
    }
    if (!data || bytes < required.bytes || reinterpret_cast<uintptr_t>(data) % required.alignment ||
        !host.monotonic_now_us || !files.submit) {
        return AYNI_INVALID_ARGUMENT;
    }
    Regions regions{static_cast<uint8_t *>(data)};
    impl_ = new (regions.take(sizeof(Impl), alignof(Impl))) Impl(host, files);
    status = impl_->bodies.configure(files);
    if (status != AYNI_OK) {
        return status;
    }
    auto config = configuration();
    size_t kernel_bytes = 0, alignment = 0;
    status = ayni_kernel_memory_requirements(&config, &kernel_bytes, &alignment);
    if (status == AYNI_OK) {
        impl_->host.bodies = impl_->bodies.store(); // Core releases the bodies it stops naming.
        status = ayni_create(&config, &impl_->host, regions.take(kernel_bytes, alignment),
                             kernel_bytes, &impl_->kernel);
    }
    if (status == AYNI_OK) {
        status = owner_workspace(regions, &impl_->owner_region);
    }
    return status == AYNI_OK ? impl_->configure() : status;
}
ayni_kernel_t *Profile::kernel() const {
    return impl_ ? impl_->kernel : nullptr;
}
uint64_t Profile::session() const {
    return impl_ ? impl_->session : 0;
}
uint64_t Profile::instance(uint32_t index) const {
    return impl_ && index < agent_count ? impl_->instances[index] : 0;
}
const ayni_body_store_v1 *Profile::bodies() const {
    return impl_ ? impl_->bodies.store() : nullptr;
}
const ayni_workspace_v1 *Profile::value_workspace() const {
    return impl_ ? &impl_->owner_region : nullptr;
}
ayni_status_t Profile::start() {
    return impl_ ? ayni_start(impl_->kernel) : AYNI_INVALID_STATE;
}
bool Profile::running() const {
    auto info = initialized<ayni_kernel_info_v1>();
    return impl_ && impl_->session && impl_->kernel &&
           ayni_get_kernel_info(impl_->kernel, &info) == AYNI_OK &&
           info.lifecycle == AYNI_LIFECYCLE_RUNNING;
}
Submission Profile::submit(ayni_intent_kind_t kind, const void *body, uint32_t revision) {
    if (impl_) {
        return Impl::command(impl_, kind, body, revision);
    }
    return {};
}
bool Profile::command_pending() const {
    return impl_ && impl_->pending_command.has_value();
}
runtime::conversation::Session *Profile::conversation(uint32_t index) {
    return impl_ && index < agent_count ? &impl_->conversations[index] : nullptr;
}
// Admission is deliberately separate from start: replay can reach RUNNING before
// the authenticated owner session is available. The OS loop keeps progressing both.
ayni_status_t Profile::tick() {
    if (!impl_ || !impl_->kernel) {
        return AYNI_INVALID_STATE;
    }
    auto &self = *impl_;
    for (auto *model : {&self.model_a, &self.model_b, &self.summary}) {
        const auto status = model->advance();
        if (status != AYNI_OK) {
            return status;
        }
    }
    auto status = self.executor.advance();
    if (status == AYNI_OK) {
        status = self.software.advance();
    }
    if (status != AYNI_OK) {
        return status;
    }
    status = ayni_step(self.kernel, step_units, step_us, nullptr);
    auto info = initialized<ayni_kernel_info_v1>();
    if (status == AYNI_OK) {
        status = ayni_get_kernel_info(self.kernel, &info);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (info.lifecycle == AYNI_LIFECYCLE_RESTRICTED || info.lifecycle == AYNI_LIFECYCLE_FENCED) {
        return AYNI_NOT_READY;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_OK;
    }
    if (!self.session) {
        const ayni_session_desc_v1 desc{sizeof(desc), AYNI_CHANNEL_MANAGEMENT, owner_principal,
                                        version, 0};
        status = ayni_open_session(self.kernel, &desc, &self.session);
        if (status == AYNI_NOT_READY) {
            return AYNI_OK;
        }
        if (status != AYNI_OK) {
            return status;
        }
        self.boot = info.boot_epoch;
        for (uint32_t at = 0; at < agent_count; ++at) {
            auto agent = initialized<ayni_agent_info_v1>();
            status = ayni_get_agent(self.kernel, at + 1U, &agent);
            if (status != AYNI_OK) {
                return status;
            }
            self.instances[at] = agent.instance_id;
        }
    }
    status = self.receipts();
    if (status != AYNI_OK) {
        return status;
    }
    // A held Task reports its status, while other Agent file/ACK bridges still
    // progress. Core scheduling is independent of this outer publication loop.
    ayni_status_t first = AYNI_OK;
    for (uint32_t at = 0; at < agent_count; ++at) {
        if (self.conversation_bound[at]) {
            const auto current = self.conversations[at].poll();
            if (first == AYNI_OK && current != AYNI_OK && current != AYNI_BUSY) {
                first = current;
            }
        }
    }
    return first;
}

ayni_status_t Profile::open_conversations() {
    if (!running()) {
        return AYNI_NOT_READY;
    }
    auto &self = *impl_;
    for (uint32_t at = 0; at < agent_count; ++at) {
        if (self.conversation_bound[at]) {
            if (!self.conversations[at].ready()) {
                return AYNI_BUSY;
            }
            continue;
        }
        auto context = initialized<ayni_context_info_v1>();
        auto status = ayni_get_context(self.kernel, self.session, self.instances[at], &context);
        if (status == AYNI_NOT_FOUND) {
            auto body = initialized<ayni_context_command_v1>();
            body.owner_instance_id = self.instances[at];
            const auto accepted = submit(AYNI_INTENT_CONTEXT_OPEN, &body);
            return accepted.status == AYNI_OK ? AYNI_BUSY : accepted.status;
        }
        if (status != AYNI_OK) {
            return status;
        }
        auto kernel_info = initialized<ayni_kernel_info_v1>();
        status = ayni_get_kernel_info(self.kernel, &kernel_info);
        if (status != AYNI_OK) {
            return status;
        }
        if (context.commit_seq > kernel_info.durable_seq) {
            return AYNI_BUSY;
        }
        if (!context.usable || context.pending_action != AYNI_CONTEXT_IDLE) {
            return AYNI_NOT_READY;
        }
        status = self.conversations[at].configure({self.kernel,
                                                   self.session,
                                                   self.instances[at],
                                                   text(chat_types[at]),
                                                   &self.owner_region,
                                                   self.values,
                                                   &self.bodies,
                                                   {&self, Impl::command}});
        if (status != AYNI_OK) {
            return status;
        }
        self.conversation_bound[at] = true;
        return AYNI_BUSY;
    }
    return AYNI_OK;
}
ayni_status_t Profile::begin_shutdown() {
    return impl_ ? ayni_begin_shutdown(impl_->kernel) : AYNI_INVALID_STATE;
}
bool Profile::stopped() const {
    auto info = initialized<ayni_kernel_info_v1>();
    return impl_ && ayni_get_kernel_info(impl_->kernel, &info) == AYNI_OK &&
           info.lifecycle == AYNI_LIFECYCLE_STOPPED;
}
ayni_status_t Profile::destroy() {
    if (!impl_) {
        return AYNI_OK;
    }
    if (impl_->model_a.pending() || impl_->model_b.pending() || impl_->summary.pending() ||
        impl_->software.pending() || impl_->executor.pending()) {
        return AYNI_BUSY;
    }
    const auto status = impl_->kernel ? ayni_destroy(impl_->kernel) : AYNI_OK;
    if (status != AYNI_OK) {
        return status;
    }
    impl_->kernel =
        nullptr; // Successful destroy ends the handle even if a later close reports BUSY.
    const auto closed = impl_->software.close();
    if (closed != AYNI_OK) {
        return closed;
    }
    const auto executor_closed = impl_->executor.close();
    if (executor_closed != AYNI_OK) {
        return executor_closed;
    }
    impl_->~Impl();
    impl_ = nullptr;
    return AYNI_OK;
}
uint64_t Profile::model_executions(uint32_t index) const {
    return impl_ && index < agent_count
               ? (index == 0 ? impl_->model_a.executions() : impl_->model_b.executions())
               : 0;
}
ExecutionEvidence Profile::execution_evidence() const {
    return impl_ ? ExecutionEvidence{impl_->executor.logical_call(), impl_->executor.samples(),
                                     impl_->executor.submissions()}
                 : ExecutionEvidence{};
}
uint64_t Profile::model_submissions() const {
    return impl_ ? impl_->model_a.submissions() + impl_->model_b.submissions() +
                       impl_->summary.submissions()
                 : 0;
}
} // namespace ayni::runtime::portable
