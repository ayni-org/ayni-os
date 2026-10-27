#include "multinode_transport.h"
#include "normal_fixture_profile.h"
#include <ayni/ayni.hpp>
#include <ayni/host_authority.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/runtime/sim_node.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace ayni::runtime::multinode {
namespace {
constexpr uint64_t owner = 1, caller_agent = 1, service_agent = 9;
constexpr uint64_t caller_tick_us = 1000, receiver_tick_us = 3000;
constexpr uint64_t receiver_offset_us = 1000000, invoke_delay_us = 100000;
constexpr uint64_t query_delay_us = 2000, effect_delay_us = 40000;
constexpr uint64_t service_command_floor = 1000;
constexpr uint32_t max_ticks = 4096, step_items = 4, step_time_us = 1000;
constexpr uint32_t goal_capacity = 8, task_capacity = 8, call_capacity = 8;
constexpr uint32_t goal_types = applications::normal_fixtures::goal_type_count;
constexpr uint32_t command_capacity = 16, checkpoint_bytes = 1;
constexpr uint32_t artifact_schema = 1, remote_operations = 2;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
// Classified first failure with a borrowed static phase label, so reporting
// the original C status does not require allocating an exception message.
class Failure : public std::exception {
  public:
    Failure(ayni_status_t value, const char *label) : status(value), label_(label) {}
    const char *what() const noexcept override { return label_; }
    ayni_status_t status;

  private:
    const char *label_;
};
void require(bool condition, const char *reason) {
    if (!condition) {
        throw Failure(AYNI_INVALID_STATE, reason);
    }
}
void check(ayni_status_t status, const char *where) {
    if (status != AYNI_OK) {
        throw Failure(status, where);
    }
}
// One artifact representation of the typed reply, shared by trace and manifest.
void write_reply(std::ostream &out, const Reply &r) {
    out << "{\"caller_node\":\"a\",\"service_node\":\"b\",\"operation\":\"" << r.operation
        << "\",\"caller_task\":\"" << r.caller_task << "\",\"caller_instance\":\""
        << r.caller_instance << "\",\"service_goal\":\"" << r.service_goal
        << "\",\"service_task\":\"" << r.service_task << "\",\"service_instance\":\""
        << r.service_instance << "\",\"acceptance_seq\":\"" << r.acceptance_seq
        << "\",\"durable_seq\":\"" << r.durable_seq << "\",\"effect\":" << r.effect
        << ",\"task_state\":" << r.task_state << ",\"completed\":" << r.completed << '}';
}
enum class Role { caller, service };
// Metadata and providers precede the node, so their lifetimes extend past its
// release. The outer Engine is retained in full when teardown cannot be proved.
class Node {
  public:
    explicit Node(Role role) : role_(role) {}
    fixtures::Counter counter{"test.increment", fixtures::Counter::Effect::increment};
    applications::normal_fixtures::Policy policy;
    SimNode sim;
    uint64_t session = 0, instance = 0, grant = 0, command = 0;
    size_t arena_bytes = 0;
    void open(const ayni_provider_v1 &offer) {
        auto config = default_config();
        config.agent_capacity = 1;
        config.goal_type_capacity = goal_types;
        config.goal_capacity = goal_capacity;
        config.task_capacity = task_capacity;
        config.strategy_capacity = 1;
        config.policy_intent_capacity = 1;
        config.task_checkpoint_max_bytes = checkpoint_bytes;
        config.command_capacity = command_capacity;
        config.memory_capacity = 0;
        config.resource_capacity = 1;
        config.resource_request_capacity = call_capacity;
        config.call_capacity = call_capacity;
        config.call_capability_capacity = 1;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.storage_write_interval_us = caller_tick_us;
        size_t alignment = 0;
        check(ayni_kernel_memory_requirements(&config, &arena_bytes, &alignment), "arena");
        check(sim.assemble(config), "assemble");
        if (role_ == Role::service) {
            sim.advance(receiver_offset_us);
        }
        const ayni_principal_desc_v1 principal{sizeof(principal), AYNI_PRINCIPAL_OWNER, owner, 1,
                                               0};
        check(ayni_register_principal(sim.kernel(), &principal), "owner");
        const ayni_agent_desc_v1 agent{sizeof(agent), 0, agent_id()};
        check(ayni_register_agent(sim.kernel(), &agent), "agent");
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text("counter.slot"), 1, 0, 0, 0, nullptr, {}};
        check(ayni_register_resource(sim.kernel(), &resource), "resource");
        check(ayni_set_resource_health(sim.kernel(), resource.resource_id, AYNI_RESOURCE_ONLINE),
              "health");
        auto domain = initialized<ayni_authority_value_set_v1>();
        domain.kind = AYNI_AUTHORITY_IDENTIFIER;
        domain.values.tokens = {&offer.destination_id, 1, 0};
        const ayni_authority_field_desc_v1 field{
            sizeof(field), applications::normal_fixtures::remote_destination_field,
            AYNI_AUTHORITY_EGRESS_DESTINATION, 0, domain};
        const bool egress = offer.destination_id.size != 0;
        const ayni_capability_desc_v1 cap{sizeof(cap),      1, text("test.increment"),
                                          egress ? 1U : 0U, 0, egress ? &field : nullptr};
        check(ayni_register_capability(sim.kernel(), &cap), "capability");
        check(ayni_register_call_capability(sim.kernel(), offer.capabilities), "call contract");
        check(ayni_register_provider(sim.kernel(), &offer), "provider");
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("increment"),
                                      cap.capability_id,
                                      1,
                                      1,
                                      offer.provider_id,
                                      offer.endpoint_id,
                                      offer.destination_id};
        check(ayni_register_binding(sim.kernel(), &binding), "binding");
        check(policy.register_all(sim.kernel(), applications::normal_fixtures::PolicyRevision::v1),
              "policy");
        check(ayni_start(sim.kernel()), "start");
    }
    uint64_t agent_id() const { return role_ == Role::caller ? caller_agent : service_agent; }
    uint64_t clock_us() const {
        const auto clock = sim.clock();
        return clock.now_us(clock.context);
    }
    ayni_kernel_info_v1 info() const {
        auto out = initialized<ayni_kernel_info_v1>();
        check(ayni_get_kernel_info(sim.kernel(), &out), "kernel info");
        return out;
    }
    void step() {
        if (!sim.kernel() || info().lifecycle == AYNI_LIFECYCLE_STOPPED ||
            info().lifecycle == AYNI_LIFECYCLE_CONFIGURING) {
            return;
        }
        auto report = initialized<ayni_step_report_v1>();
        check(ayni_step(sim.kernel(), step_items, step_time_us, &report), "step");
        require(report.items_done <= step_items, "step item budget");
        const auto state = info().lifecycle;
        require(state != AYNI_LIFECYCLE_RESTRICTED && state != AYNI_LIFECYCLE_FENCED, "restricted");
    }
    ayni_status_t authenticate() {
        if (grant) {
            return AYNI_OK;
        }
        const ayni_session_desc_v1 desc{sizeof(desc), AYNI_CHANNEL_MANAGEMENT, owner, 1, 0};
        const auto status = ayni_open_session(sim.kernel(), &desc, &session);
        if (status == AYNI_NOT_READY) {
            return status;
        }
        check(status, "session");
        auto out = initialized<ayni_agent_info_v1>();
        check(ayni_get_agent(sim.kernel(), agent_id(), &out), "instance");
        instance = out.instance_id;
        const ayni_scope_v1 full{sizeof(full), 0, nullptr};
        const ayni_grant_create_v1 body{
            sizeof(body), 1, text("test.increment"), instance, 0, AYNI_GRANT_PERMANENT, 0, 0, full};
        grant = submit(AYNI_INTENT_GRANT_CREATE, body).object.id;
        return AYNI_OK;
    }
    template <class Body> ayni_intent_result_v1 submit(ayni_intent_kind_t kind, const Body &body) {
        require(command + 1 < service_command_floor, "local command namespace");
        const ayni_intent_v1 intent{sizeof(intent), kind, ++command, session, 0, 0, &body};
        auto out = initialized<ayni_intent_result_v1>();
        auto error = initialized<ayni_error_v1>();
        check(ayni_submit_intent(sim.kernel(), &intent, &out, &error), "management command");
        return out;
    }
    uint64_t create(applications::normal_fixtures::GoalKind kind) {
        using applications::normal_fixtures::GoalKind;
        require(kind == GoalKind::local || kind == GoalKind::remote, "fixture Goal kind");
        const bool local = kind == GoalKind::local;
        auto body = initialized<ayni_goal_create_v1>();
        body.goal_type_version = 1;
        body.goal_type_id = text(local ? "fixture.local" : "fixture.remote");
        body.owner_instance_id = instance;
        body.trigger = local ? AYNI_TRIGGER_LOCAL_INPUT : AYNI_TRIGGER_ONCE;
        if (local) {
            body.trigger_source = text("sim.local");
        }
        return submit(AYNI_INTENT_GOAL_CREATE, body).object.id;
    }
    void input() {
        auto event = initialized<ayni_event_v1>();
        event.kind = AYNI_EVENT_LOCAL_INPUT;
        event.source = text("sim.local");
        check(ayni_post_event(sim.kernel(), &event), "local input");
    }
    ayni_goal_info_v1 goal(uint64_t id) const {
        auto out = initialized<ayni_goal_info_v1>();
        check(ayni_get_goal(sim.kernel(), id, &out), "goal");
        return out;
    }
    ayni_task_info_v1 task(uint64_t goal_id) const {
        auto out = initialized<ayni_task_info_v1>();
        const auto id = goal(goal_id).last_task_id;
        if (id) {
            check(ayni_get_task(sim.kernel(), id, &out), "task");
        }
        return out;
    }
    bool drained() const {
        return !sim.kernel() || info().lifecycle == AYNI_LIFECYCLE_STOPPED ||
               info().lifecycle == AYNI_LIFECYCLE_CONFIGURING;
    }
    bool completed(uint64_t goal_id) const {
        const auto out = task(goal_id);
        return out.task_id && out.lifecycle == AYNI_TASK_COMPLETED &&
               out.commit_seq <= info().durable_seq;
    }
    void check_grant(uint64_t goal_id) const {
        auto authority = initialized<ayni_goal_authority_info_v1>();
        auto requirement = initialized<ayni_goal_requirement_info_v1>();
        check(ayni_get_goal_authority(sim.kernel(), goal_id, &authority), "Goal authority");
        check(ayni_get_goal_requirement(sim.kernel(), goal_id, 0, &requirement), "Goal Grant");
        require(authority.phase == AYNI_GOAL_AUTHORITY_AUTHORIZED &&
                    authority.requirement_count == 1 && authority.missing_count == 0 &&
                    requirement.grant_id == grant && requirement.grant_revision == 1 &&
                    requirement.effective_reason == AYNI_REASON_NONE,
                "local selected Grant");
    }
    void trace_calls(std::ostream &out, const char *node, uint64_t tick) const {
        if (info().lifecycle != AYNI_LIFECYCLE_RUNNING) {
            return;
        }
        std::array<uint64_t, call_capacity> ids{};
        uint32_t count = 0;
        check(ayni_list_objects(sim.kernel(), AYNI_OBJECT_CALL, ids.data(),
                                static_cast<uint32_t>(ids.size()), &count),
              "trace calls");
        for (uint32_t at = 0; at < count; ++at) {
            auto call = initialized<ayni_call_info_v1>();
            check(ayni_get_call(sim.kernel(), ids[at], &call), "trace call");
            out << "{\"kind\":\"call\",\"tick\":" << tick << ",\"node\":\"" << node
                << "\",\"call\":\"" << call.logical_call_id << "\",\"task\":\"" << call.task_id
                << "\",\"operation\":\"" << call.operation_id << "\",\"requires_seq\":\""
                << call.requires_seq << "\",\"phase\":" << call.phase
                << ",\"outcome\":" << call.outcome << ",\"status\":" << call.status
                << ",\"reason\":" << call.reason << ",\"effect\":" << call.effect << "}\n";
        }
    }

  private:
    Role role_;
};

class Engine {
  public:
    FakeTransport transport;
    Node a{Role::caller}, b{Role::service};
    std::ofstream trace;
    std::array<Reply, remote_operations> results{};
    struct PendingOperation {
        uint32_t index;
        uint64_t goal, task, operation;
    };
    std::optional<PendingOperation> final_operation;
    bool inflight_close = false;
    uint64_t now = 0, ticks = 0, effect_at = 0, barrier_checks = 0;
    uint64_t local_a = 0, local_b = 0, local_a_task = 0;
    bool connected = false, independence = false, responsive = false;
    const char *first_failure = nullptr;
    ayni_status_t close_status = AYNI_OK;
    const char *close_detail = "none";
    enum class Closing { open, draining, complete, retained };
    Closing closing = Closing::open;
    uint64_t tick_bound = max_ticks;
    static uint64_t clock(void *ctx) { return static_cast<Engine *>(ctx)->now; }
    void tick() {
        require(++ticks <= tick_bound, "tick limit");
        now += caller_tick_us;
        a.sim.advance(caller_tick_us);
        b.sim.advance(receiver_tick_us);
        if (connected) {
            check(transport.advance(), "transport");
        }
        if (b.counter.pending()) {
            if (!effect_at) {
                effect_at = now + effect_delay_us;
            }
            if (now >= effect_at) {
                check(b.counter.advance(b.sim.kernel(), barrier_checks), "counter");
            }
        } else {
            effect_at = 0;
        }
        a.step();
        b.step();
        if (trace.is_open() && a.sim.kernel() && b.sim.kernel()) {
            trace << "{\"kind\":\"clock\",\"tick\":" << ticks << ",\"transport_us\":\"" << now
                  << "\",\"a_us\":\"" << a.clock_us() << "\",\"b_us\":\"" << b.clock_us()
                  << "\",\"a_seq\":\"" << a.info().durable_seq << "\",\"b_seq\":\""
                  << b.info().durable_seq << "\",\"received\":" << transport.received()
                  << ",\"delivered\":" << transport.delivered()
                  << ",\"effects\":" << b.counter.executions() << "}\n";
            require(trace.good(), "trace output");
            a.trace_calls(trace, "a", ticks);
            b.trace_calls(trace, "b", ticks);
        }
    }
    template <class Condition> void until(Condition condition) {
        for (uint32_t round = 0; round < max_ticks; ++round) {
            if (condition()) {
                return;
            }
            tick();
        }
        throw Failure(AYNI_INVALID_STATE, "condition limit");
    }
    void run() {
        a.open(transport.describe());
        b.open(b.counter.describe());
        until([&] {
            return a.info().lifecycle == AYNI_LIFECYCLE_RUNNING &&
                   b.info().lifecycle == AYNI_LIFECYCLE_RUNNING;
        });
        until([&] { return a.authenticate() == AYNI_OK && b.authenticate() == AYNI_OK; });
        // Before any remote messages, prove progressing A's local storage/time
        // alone does not advance B's commit prefix or its independent clock.
        const auto b_before = b.info().durable_seq;
        const auto b_time = b.clock_us();
        local_a = a.create(applications::normal_fixtures::GoalKind::local);
        a.input();
        for (uint32_t at = 0; at < max_ticks && !a.completed(local_a); ++at) {
            a.sim.advance(caller_tick_us);
            a.step();
        }
        require(a.completed(local_a) && b.info().durable_seq == b_before && b.clock_us() == b_time,
                "independent node storage/time");
        independence = true;
        local_a_task = a.task(local_a).task_id;
        check(transport.connect({a.sim.kernel(), b.sim.kernel(), a.instance, b.instance, b.session,
                                 service_command_floor, "fixture.action", this, clock,
                                 invoke_delay_us, query_delay_us}),
              "connect");
        connected = true;
        for (uint32_t index = 0; index < remote_operations; ++index) {
            invoke(index);
        }
        require(final_operation.has_value() && transport.pending() && b.counter.pending(),
                "inflight shutdown setup");
        inflight_close = true;
    }
    Reply query(uint64_t operation) {
        check(transport.query(operation), "query submit");
        Reply reply{};
        ayni_status_t status = AYNI_NOT_READY;
        until([&] {
            status = transport.take_reply(reply);
            return status != AYNI_NOT_READY;
        });
        check(status, "query reply");
        trace << "{\"kind\":\"query_reply\",\"tick\":" << ticks << ",\"reply\":";
        write_reply(trace, reply);
        trace << "}\n";
        require(trace.good(), "query artifact");
        return reply;
    }
    void invoke(uint32_t index) {
        const auto previous_operation = transport.last_operation();
        const auto goal_id = a.create(applications::normal_fixtures::GoalKind::remote);
        until([&] { return transport.last_operation() != previous_operation; });
        const auto operation = transport.last_operation();
        const auto original = a.task(goal_id);
        a.check_grant(goal_id);
        require(original.task_id && original.owner_instance_id == a.instance, "caller owner");
        if (index == 0) {
            local_b = b.create(applications::normal_fixtures::GoalKind::local);
            a.input();
            b.input();
            until([&] {
                return a.task(local_a).task_id != local_a_task && a.completed(local_a) &&
                       b.completed(local_b);
            });
            require(transport.delivered() == 0 && b.counter.executions() == 0 &&
                        a.task(goal_id).lifecycle == AYNI_TASK_RUNNING,
                    "local responsiveness");
            responsive = true;
        }
        until([&] { return b.counter.pending(); });
        b.check_grant(transport.last_service_goal());
        const auto in_progress = query(operation);
        require(!in_progress.completed && in_progress.service_task &&
                    in_progress.task_state == AYNI_TASK_RUNNING &&
                    in_progress.caller_task == original.task_id &&
                    in_progress.caller_instance == a.instance &&
                    in_progress.service_instance == b.instance &&
                    in_progress.acceptance_seq <= in_progress.durable_seq,
                "in-progress peer query");
        const PendingOperation pending{index, goal_id, original.task_id, operation};
        if (index + 1 == remote_operations) {
            final_operation = pending;
        } else {
            finish_operation(pending);
        }
    }
    void finish_operation(const PendingOperation &pending) {
        until([&] { return a.completed(pending.goal); });
        const auto reply = query(pending.operation);
        const auto task = a.task(pending.goal);
        require(reply.completed && reply.effect == AYNI_CALL_EFFECT_COMPLETE &&
                    reply.caller_task == pending.task && task.task_id == pending.task &&
                    task.owner_instance_id == a.instance && reply.service_instance == b.instance &&
                    reply.service_goal == transport.last_service_goal(),
                "completed peer query and fixed owners");
        if (pending.index) {
            const auto &previous = results[pending.index - 1];
            require(reply.operation != previous.operation &&
                        reply.service_goal != previous.service_goal &&
                        reply.service_task != previous.service_task,
                    "independent operations");
        }
        results[pending.index] = reply;
    }
    bool close() noexcept {
        if (closing == Closing::complete) {
            return true;
        }
        if (closing == Closing::retained) {
            return false;
        }
        closing = Closing::draining;
        tick_bound = ticks + max_ticks;
        try {
            // Service policies must still run to publish completion. Both
            // Kernels enter DRAINING only after accepted invokes have settled;
            // a read-only simulated query holds no external effect or borrow.
            transport.cancel_query();
            if (connected && a.sim.kernel() && b.sim.kernel() &&
                a.info().lifecycle == AYNI_LIFECYCLE_RUNNING &&
                b.info().lifecycle == AYNI_LIFECYCLE_RUNNING) {
                until([&] { return !transport.pending() && !b.counter.pending(); });
                if (final_operation) {
                    finish_operation(*final_operation);
                    final_operation.reset();
                    require(b.counter.executions() == remote_operations &&
                                transport.delivered() == remote_operations,
                            "execution count");
                }
            }
            for (auto *node : {&a, &b}) {
                if (!node->drained()) {
                    check(ayni_begin_shutdown(node->sim.kernel()), "shutdown");
                }
            }
            until([&] { return a.drained() && b.drained(); });
            require(!transport.pending() && !b.counter.pending(), "producer quiescence");
            for (auto *node : {&a, &b}) {
                if (node->sim.kernel()) {
                    check(node->sim.release(), "release");
                }
            }
            closing = Closing::complete;
            return true;
        } catch (const Failure &failure) {
            return retain(failure.status, failure.what());
        } catch (const std::bad_alloc &) {
            return retain(AYNI_CAPACITY_EXCEEDED, "close allocation");
        } catch (...) {
            return retain(AYNI_INTERNAL, "close exception");
        }
    }
    bool retain(ayni_status_t status, const char *detail) noexcept {
        closing = Closing::retained;
        close_status = status;
        close_detail = detail;
        if (!first_failure) {
            first_failure = detail;
        }
        return false;
    }
    void write(std::ostream &out, const fixtures::Result &result) const {
        out << "{\"schema\":" << artifact_schema
            << ",\"case_id\":\"MA038-N1\",\"source_revision\":\"" << fixtures::source_revision()
            << "\",\"build_type\":\"" << AYNI_FIXTURE_BUILD_TYPE << "\",\"sanitizer\":\""
            << AYNI_FIXTURE_SANITIZER << "\",\"compiler\":\"" << __VERSION__
            << "\",\"status\":" << result.status << ",\"close_status\":" << result.close_status
            << ",\"first_failure\":\"" << (first_failure ? first_failure : "none")
            << "\",\"independent\":" << independence << ",\"responsive\":" << responsive
            << ",\"inflight_close\":" << inflight_close << ",\"caller_agent\":\"" << caller_agent
            << "\",\"service_agent\":\"" << service_agent
            << "\",\"caller_arena_bytes\":" << a.arena_bytes
            << ",\"receiver_arena_bytes\":" << b.arena_bytes
            << ",\"invoke_delay_us\":" << invoke_delay_us
            << ",\"query_delay_us\":" << query_delay_us
            << ",\"effect_delay_us\":" << effect_delay_us
            << ",\"caller_tick_us\":" << caller_tick_us
            << ",\"receiver_tick_us\":" << receiver_tick_us
            << ",\"receiver_offset_us\":" << receiver_offset_us << ",\"step_items\":" << step_items
            << ",\"step_time_us\":" << step_time_us << ",\"effects\":" << b.counter.executions()
            << ",\"barrier_checks\":" << barrier_checks << ",\"operations\":[";
        for (uint32_t at = 0; at < results.size(); ++at) {
            const auto &r = results[at];
            if (at) {
                out << ',';
            }
            write_reply(out, r);
        }
        out << "]}\n";
    }
};
struct RetainUntilClosed {
    void operator()(Engine *engine) const noexcept {
        if (engine && engine->close()) {
            delete engine;
        }
    }
};
} // namespace
} // namespace ayni::runtime::multinode

namespace ayni::runtime::fixtures {
namespace {
// Diagnostic text is optional; classified machine status and the Engine's
// static first_failure remain authoritative when allocating a string fails.
void record_failure(Result &result, multinode::Engine *engine, ayni_status_t status,
                    const char *label) noexcept {
    if (engine && !engine->first_failure) {
        engine->first_failure = label;
    }
    if (result.status != AYNI_OK) {
        return;
    }
    result.status = status;
    try {
        result.detail = label;
    } catch (...) { /* Optional diagnostic only. */
    }
}
} // namespace
Result run_multinode(const std::string &output_directory) {
    Result result;
    std::unique_ptr<multinode::Engine, multinode::RetainUntilClosed> engine;
    try {
        std::error_code error;
        if (!std::filesystem::create_directory(output_directory, error)) {
            result.artifact_status = AYNI_STORAGE_FAILED;
            record_failure(result, nullptr, AYNI_STORAGE_FAILED, "output directory must be new");
            return result;
        }
        engine.reset(new multinode::Engine);
        engine->trace.open(std::filesystem::path(output_directory) / "trace.jsonl");
        if (!engine->trace) {
            throw multinode::Failure(AYNI_STORAGE_FAILED, "open trace");
        }
        engine->run();
    } catch (const multinode::Failure &failure) {
        record_failure(result, engine.get(), failure.status, failure.what());
    } catch (const std::bad_alloc &) {
        record_failure(result, engine.get(), AYNI_CAPACITY_EXCEEDED, "allocation");
    } catch (const std::filesystem::filesystem_error &) {
        record_failure(result, engine.get(), AYNI_STORAGE_FAILED, "filesystem");
    } catch (...) {
        record_failure(result, engine.get(), AYNI_INTERNAL, "unexpected exception");
    }
    if (!engine) {
        result.artifact_status = result.status;
        return result;
    }
    if (!engine->close()) {
        result.close_status = engine->close_status;
        record_failure(result, engine.get(), result.close_status, engine->close_detail);
    }
    try {
        engine->trace.close();
        std::ofstream out(std::filesystem::path(output_directory) / "run.json");
        engine->write(out, result);
        out.close();
        if (!out || engine->trace.fail()) {
            throw multinode::Failure(AYNI_STORAGE_FAILED, "artifact write");
        }
    } catch (const std::bad_alloc &) {
        result.artifact_status = AYNI_CAPACITY_EXCEEDED;
        record_failure(result, engine.get(), result.artifact_status, "artifact allocation");
    } catch (...) {
        result.artifact_status = AYNI_STORAGE_FAILED;
        record_failure(result, engine.get(), result.artifact_status, "artifacts");
    }
    return result;
}
} // namespace ayni::runtime::fixtures
