#include <atomic>
#include <ayni/applications/software_workflow.h>
#include <ayni/host_authority.h>
#include <ayni/hosts/native.h>
#include <ayni/hosts/sim.h>
#include <ayni/providers/openai_compat.h>
#include <ayni/providers/openai_compat/loopback.h>
#include <ayni/providers/software/work.h>
#include <ayni/runtime/common/demo_profile.h>
#include <ayni/runtime/common/memory_bodies.h>
#include <ayni/runtime/examples.h>
#include <ayni/workspace.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
namespace ayni::runtime::examples {
namespace {
constexpr uint32_t step_units = 64, step_max_us = 10000, drain_steps = 1024;
constexpr uint64_t tick_us = 100;
// Fixture sampling need not model a wall-clock second. One event takes 100
// simulated advances, keeping the same wait/freshness semantics under sanitizers.
constexpr uint64_t software_event_interval_us = 10000;
constexpr int64_t fixture_utc_s = 86400;
constexpr auto wait_timeout = std::chrono::seconds(15);
constexpr auto owner_yield = std::chrono::microseconds(50);
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
ayni_scope_v1 empty_scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
} // namespace
struct Fixture::Impl {
    hosts::sim::Host host;
    std::unique_ptr<hosts::native::Host> native_host; // Present only for the native profile.
    ayni_host_v1 host_ports{}; // Borrowed from the selected Host through successful close.
    MemoryBodies bodies;       // Outlives the profile that borrows it.
    DemoProfile profile;
    providers::openai_compat::Provider alternate;
    std::unique_ptr<providers::openai_compat::fixtures::LoopbackHttp> server;
    std::optional<ayni_provider_v1> alternate_offer;
    ayni_scope_field_v1 destination{};
    Extension extension;
    ayni::Workspace software_workspace; // Borrowed by the software-workflow strategy.
    providers::software::WorkProvider software;
    bool software_enabled = false; // Concrete composition choice, fixed before Kernel creation.
    std::atomic<uint64_t> clock_us{0};
    std::atomic<uint32_t> writes{0};
    uint64_t session = 0, command = 0;
    ayni::Kernel kernel; // Last: descriptors, ports and Host outlive its explicit close.

    static uint64_t now(void *ctx) { return static_cast<Impl *>(ctx)->clock_us.load(); }
    static void utc(void *, ayni_utc_sample_v1 *out) {
        *out = {sizeof(*out), AYNI_UTC_TRUSTED, fixture_utc_s};
    }
    static ayni_status_t present(void *ctx, ayni_string_v1) {
        ++static_cast<Impl *>(ctx)->writes;
        return AYNI_OK; // Explicit fixture sink, not evidence of a physical display.
    }
    static ayni_status_t progress(void *ctx) {
        auto &self = *static_cast<Impl *>(ctx);
        if (!self.native_host) {
            self.host.advance_us(tick_us);
            self.host.complete_storage();
        }
        self.clock_us.store(self.host_ports.monotonic_now_us(self.host_ports.ctx));
        auto status = self.profile.advance();
        if (status == AYNI_OK && self.alternate_offer) {
            status = self.alternate.status();
        }
        if (status == AYNI_OK && self.software_enabled) {
            status = self.software.advance();
        }
        if (status == AYNI_OK && self.extension.advance) {
            status = self.extension.advance(self.extension.context, self.clock_us.load());
        }
        std::this_thread::sleep_for(owner_yield); // Yield to real protocol/presentation workers.
        return status;
    }
    static ayni_status_t quiesce(void *ctx) {
        auto &self = *static_cast<Impl *>(ctx);
        auto status = self.profile.quiesce();
        if (status == AYNI_OK && self.alternate_offer) {
            status = self.alternate.quiesce();
        }
        if (status == AYNI_OK && self.extension.quiesce) {
            status = self.extension.quiesce(self.extension.context);
        }
        if (status == AYNI_OK && self.native_host) {
            // C-HOST-018: the SDK calls this only in CONFIGURING/STOPPED, before destroy.
            self.native_host->close_storage();
        }
        return status;
    }
    ayni_status_t select_host(HostConfig selection, uint32_t completion_slots) {
        if (selection.kind == HostConfig::Kind::sim) {
            host.set_utc(fixture_utc_s);
            host_ports = host.table();
            host_ports.bodies = bodies.store();
            return AYNI_OK;
        }
        native_host = std::make_unique<hosts::native::Host>();
        const auto opened = native_host->open_storage(selection.state_directory, completion_slots);
        using Status = hosts::native::StorageStatus;
        switch (opened.status) {
        case Status::ok:
            host_ports = native_host->table();
            host_ports.bodies = bodies.store();
            clock_us.store(host_ports.monotonic_now_us(host_ports.ctx));
            return AYNI_OK;
        case Status::invalid_argument:
            return AYNI_INVALID_ARGUMENT;
        case Status::already_open:
            return AYNI_INVALID_STATE;
        case Status::no_memory:
        case Status::thread_error:
            return AYNI_CAPACITY_EXCEEDED;
        case Status::in_use:
            return AYNI_BUSY;
        case Status::not_a_log:
            return AYNI_UNSUPPORTED_VERSION;
        case Status::file_error:
            return AYNI_STORAGE_FAILED;
        }
        return AYNI_INTERNAL;
    }
    ayni_status_t register_alternate() {
        server = std::make_unique<providers::openai_compat::fixtures::LoopbackHttp>(
            R"({"id":"fixture","choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"alternate fixture reply"}}]})");
        providers::openai_compat::Config config;
        config.base_url = server->url() + "/v1";
        config.model = "fixture-second-model";
        config.provider_id = "example.openai.second";
        config.endpoint_id = "example.openai.chat";
        config.destination_id = "example.openai.destination";
        config.key_environment = "AYNI_EXAMPLE_FIXTURE_KEY";
        config.result_format = AYNI_CALL_RESULT_TEXT;
        config.timeout_ms = 2000;
        config.bodies = bodies.store();
        // Child process fixture only: no secret, external service or persistent config.
        if (setenv(config.key_environment.c_str(), "fixture-only", 1) != 0) {
            return AYNI_INTERNAL;
        }
        auto status = alternate.configure(config);
        unsetenv(config.key_environment.c_str());
        if (status != AYNI_OK) {
            return status;
        }
        ayni_provider_v1 offer{};
        status = alternate.describe(offer);
        if (status != AYNI_OK) {
            return status;
        }
        alternate_offer = offer;
        const auto &saved = *alternate_offer;
        ayni_authority_value_set_v1 domain{};
        domain.struct_size = sizeof(domain);
        domain.kind = AYNI_AUTHORITY_IDENTIFIER;
        domain.values.tokens = {&saved.destination_id, 1, 0};
        const ayni_authority_field_desc_v1 field{sizeof(field), 1,
                                                 AYNI_AUTHORITY_EGRESS_DESTINATION, 0, domain};
        const ayni_capability_desc_v1 capability{
            sizeof(capability), 1, saved.capabilities->capability_id, 1, 0, &field};
        status = ayni_register_capability(kernel.get(), &capability);
        if (status == AYNI_OK) {
            status = ayni_register_call_capability(kernel.get(), saved.capabilities);
        }
        if (status == AYNI_OK) {
            status = ayni_register_provider(kernel.get(), &saved);
        }
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("alternate"),
                                      capability.capability_id,
                                      1,
                                      1,
                                      saved.provider_id,
                                      saved.endpoint_id,
                                      saved.destination_id,
                                      0,
                                      0,
                                      nullptr};
        if (status == AYNI_OK) {
            status = ayni_register_binding(kernel.get(), &binding);
        }
        destination = {};
        destination.struct_size = sizeof(destination);
        destination.field_id = 1;
        destination.allowed = domain;
        return status;
    }
    ayni_status_t register_software() {
        providers::software::WorkConfig config;
        config.clock_context = this;
        config.now_us = now;
        config.event_interval_us = software_event_interval_us;
        auto status = software.configure(config);
        auto *handle = kernel.get();
        const ayni_resource_desc_v1 resource{sizeof(resource),
                                             AYNI_RESOURCE_SHARED,
                                             text("software.slot"),
                                             config.capacity,
                                             0,
                                             0,
                                             0,
                                             nullptr,
                                             {}};
        if (status == AYNI_OK) {
            status = ayni_register_resource(handle, &resource);
        }
        if (status == AYNI_OK) {
            status = ayni_set_resource_health(handle, resource.resource_id, AYNI_RESOURCE_ONLINE);
        }
        ayni_observation_source_v1 source{};
        if (status == AYNI_OK) {
            status = software.describe_source(source);
        }
        if (status == AYNI_OK) {
            status = ayni_register_observation_source(handle, &source);
        }
        ayni_provider_v1 provider{};
        if (status == AYNI_OK) {
            status = software.describe(provider);
        }
        for (uint32_t at = 0; status == AYNI_OK && at < provider.capability_count; ++at) {
            const auto &offer = provider.capabilities[at];
            const ayni_capability_desc_v1 capability{
                sizeof(capability), 1, offer.capability_id, 0, 0, nullptr};
            status = ayni_register_capability(handle, &capability);
            if (status == AYNI_OK) {
                status = ayni_register_call_capability(handle, &offer);
            }
        }
        if (status == AYNI_OK) {
            status = ayni_register_provider(handle, &provider);
        }
        for (uint32_t at = 0; status == AYNI_OK && at < provider.capability_count; ++at) {
            const auto id = provider.capabilities[at].capability_id;
            const ayni_binding_v1 binding{
                sizeof(binding),      1,  id, id, 1,      1, provider.provider_id,
                provider.endpoint_id, {}, 0,  0,  nullptr};
            status = ayni_register_binding(handle, &binding);
            ayni_tool_usage_v1 usage{};
            if (status == AYNI_OK) {
                status = software.usage(id, {id, binding.version, 0}, usage);
            }
            if (status == AYNI_OK) {
                status = ayni_register_capability_usage(handle, usage.purpose, &usage);
            }
        }
        return status;
    }
    ayni_status_t open(Extension ports, Model model, Software selection,
                       HostConfig host_selection) {
        software_enabled = selection == Software::work;
        extension = ports;
        auto config = demo_config();
        config.goal_type_capacity += 2;
        config.strategy_capacity += 2;
        config.call_capability_capacity += 2;
        config.capability_capacity += 2;
        config.provider_capacity += 2;
        config.binding_capacity += 2;
        config.resource_capacity += 1;
        config.observation_source_capacity += 1;
        if (software_enabled) {
            config.call_capability_capacity += 2;
            config.capability_capacity += 2;
            config.provider_capacity += 1;
            config.binding_capacity += 2;
            config.resource_capacity += 1;
            config.observation_source_capacity += 1;
        }
        auto status = select_host(host_selection, config.completion_slot_count);
        if (status == AYNI_OK) {
            status = kernel.create(config, host_ports, {this, quiesce});
        }
        if (status != AYNI_OK) {
            return status;
        }
        status = profile.assemble(kernel.get(), {},
                                  {{this, now, utc}, {this, present}, {}, {}, {}, bodies.store()});
        if (status == AYNI_OK && model == Model::alternate_loopback) {
            status = register_alternate();
        }
        if (status == AYNI_OK && software_enabled) {
            status = register_software();
        }
        if (status == AYNI_OK && software_enabled) {
            status = software_workspace.create(ayni::Workspace::Purpose::value);
            if (status == AYNI_OK) {
                status = applications::software_workflow::configure(
                    const_cast<ayni_workspace_v1 *>(software_workspace.get()), kernel.get());
            }
        }
        if (status == AYNI_OK && extension.configure) {
            status = extension.configure(extension.context, kernel.get());
        }
        if (status != AYNI_OK) {
            return status;
        }
        status = kernel.start();
        if (status != AYNI_OK) {
            return status;
        }
        const auto end = std::chrono::steady_clock::now() + wait_timeout;
        const ayni_session_desc_v1 descriptor{sizeof(descriptor), AYNI_CHANNEL_MANAGEMENT,
                                              demo_owner, demo_binding_revision, 0};
        do {
            status = progress(this);
            auto report = initialized<ayni_step_report_v1>();
            if (status == AYNI_OK) {
                status = kernel.step(step_units, step_max_us, report);
            }
            if (status != AYNI_OK) {
                return status;
            }
            status = ayni_open_session(kernel.get(), &descriptor, &session);
            if (status == AYNI_OK) {
                return status;
            }
            if (status != AYNI_NOT_READY) {
                return status;
            }
        } while (std::chrono::steady_clock::now() < end);
        return AYNI_BUSY;
    }
    template <class T>
    ayni_status_t submit(ayni_intent_kind_t kind, const T &body, uint64_t *id = nullptr) {
        if (command == UINT64_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const auto accepted = kernel.submit_raw(kind, ++command, session, 0, body);
        if (accepted.status != AYNI_OK) {
            return accepted.status;
        }
        if (id) {
            *id = accepted.result.object.id;
        }
        const auto end = std::chrono::steady_clock::now() + wait_timeout;
        do {
            auto receipt = initialized<ayni_intent_receipt_v1>();
            auto status = ayni_get_intent_receipt(kernel.get(), demo_owner, command, &receipt);
            if (status != AYNI_OK) {
                return status;
            }
            if (receipt.phase == AYNI_INTENT_DURABLE) {
                return AYNI_OK;
            }
            status = progress(this);
            auto report = initialized<ayni_step_report_v1>();
            if (status == AYNI_OK) {
                status = kernel.step(step_units, step_max_us, report);
            }
            if (status != AYNI_OK) {
                return status;
            }
        } while (std::chrono::steady_clock::now() < end);
        return AYNI_BUSY;
    }
};
Fixture::Fixture() = default;
Fixture::~Fixture() {
    if (impl_ && close() != AYNI_OK) {
        static_cast<void>(impl_.release());
    }
}
ayni_status_t Fixture::open(Extension extension, Model model, Software selection, HostConfig host) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if ((model != Model::mock && model != Model::alternate_loopback) ||
        (selection != Software::none && selection != Software::work) ||
        (host.kind != HostConfig::Kind::sim && host.kind != HostConfig::Kind::native) ||
        (host.kind == HostConfig::Kind::sim && host.state_directory) ||
        (host.kind == HostConfig::Kind::native &&
         (!host.state_directory || !host.state_directory[0]))) {
        return AYNI_INVALID_ARGUMENT;
    }
    try {
        impl_ = std::make_unique<Impl>();
        return impl_->open(extension, model, selection, host);
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    } catch (const std::exception &) {
        return AYNI_INTERNAL;
    }
}
uint64_t Fixture::instance() const {
    if (!impl_) {
        return 0;
    }
    auto info = initialized<ayni_agent_info_v1>();
    return ayni_get_agent(impl_->kernel.get(), demo_assistant, &info) == AYNI_OK ? info.instance_id
                                                                                 : 0;
}
uint64_t Fixture::now_us() const {
    return impl_ ? impl_->clock_us.load() : 0;
}
ayni_string_v1 Fixture::capability(const char *alias) const {
    if (!impl_ || !alias) {
        return {};
    }
    return std::strcmp(alias, "alternate") == 0 && impl_->alternate_offer
               ? impl_->alternate_offer->capabilities->capability_id
               : impl_->profile.capability(alias);
}
ayni_scope_v1 Fixture::scope(const char *alias) const {
    if (!impl_ || !alias) {
        return empty_scope();
    }
    return std::strcmp(alias, "alternate") == 0 && impl_->alternate_offer
               ? ayni_scope_v1{sizeof(ayni_scope_v1), 1, &impl_->destination}
               : impl_->profile.scope(alias);
}
ayni_status_t Fixture::grant(ayni_string_v1 capability, ayni_scope_v1 scope) {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    const ayni_grant_create_v1 body{sizeof(body),         1, capability, instance(), 0,
                                    AYNI_GRANT_PERMANENT, 0, 0,          scope};
    return impl_->submit(AYNI_INTENT_GRANT_CREATE, body);
}
ayni_status_t Fixture::create(const ayni_goal_create_v1 &body, uint64_t &id) {
    return impl_ ? impl_->submit(AYNI_INTENT_GOAL_CREATE, body, &id) : AYNI_INVALID_STATE;
}
ayni_status_t Fixture::post(const ayni_event_v1 &event) {
    return impl_ ? ayni_post_event(impl_->kernel.get(), &event) : AYNI_INVALID_STATE;
}
ayni_status_t Fixture::step() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    const auto status = Impl::progress(impl_.get());
    if (status != AYNI_OK) {
        return status;
    }
    auto report = initialized<ayni_step_report_v1>();
    return impl_->kernel.step(step_units, step_max_us, report);
}
ayni_status_t Fixture::wait_task(uint64_t id) {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    const auto end = std::chrono::steady_clock::now() + wait_timeout;
    do {
        auto goal = initialized<ayni_goal_info_v1>();
        auto status = ayni_get_goal(impl_->kernel.get(), id, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.lifecycle == AYNI_GOAL_PROPOSED) {
            return AYNI_DENIED;
        }
        if (goal.last_task_id) {
            auto task = initialized<ayni_task_info_v1>();
            status = ayni_get_task(impl_->kernel.get(), goal.last_task_id, &task);
            if (status != AYNI_OK) {
                return status;
            }
            auto info = initialized<ayni_kernel_info_v1>();
            status = ayni_get_kernel_info(impl_->kernel.get(), &info);
            if (status != AYNI_OK) {
                return status;
            }
            if (task.lifecycle == AYNI_TASK_COMPLETED && task.commit_seq <= info.durable_seq) {
                return AYNI_OK;
            }
            if (task.lifecycle == AYNI_TASK_PAUSED && task.hold_status != AYNI_OK) {
                return task.hold_status;
            }
            if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_CANCELLED ||
                task.lifecycle == AYNI_TASK_EXPIRED) {
                return AYNI_INVALID_STATE;
            }
        }
        status = step();
        if (status != AYNI_OK) {
            return status;
        }
    } while (std::chrono::steady_clock::now() < end);
    return AYNI_BUSY;
}
const ayni_body_store_v1 *Fixture::bodies() const {
    return impl_ ? impl_->bodies.store() : nullptr;
}
uint32_t Fixture::model_requests() const {
    return impl_ && impl_->server ? impl_->server->requests() : 0;
}
uint32_t Fixture::notifications() const {
    return impl_ ? impl_->writes.load() : 0;
}
ayni_status_t Fixture::close() {
    if (!impl_) {
        return AYNI_OK;
    }
    if (impl_->kernel.get()) {
        const auto end = std::chrono::steady_clock::now() + wait_timeout;
        ayni_status_t status = AYNI_BUSY;
        do {
            status = impl_->kernel.drain(drain_steps, {impl_.get(), Impl::progress});
        } while (status == AYNI_BUSY && std::chrono::steady_clock::now() < end);
        if (status != AYNI_OK) {
            return status;
        }
        status = impl_->kernel.close();
        if (status != AYNI_OK) {
            return status;
        }
    }
    auto status = impl_->profile.close();
    if (status == AYNI_OK && impl_->alternate_offer) {
        status = impl_->alternate.close();
    }
    if (status == AYNI_OK && impl_->software_enabled) {
        status = impl_->software.close();
    }
    if (status != AYNI_OK) {
        return status;
    }
    impl_.reset();
    return AYNI_OK;
}
} // namespace ayni::runtime::examples
