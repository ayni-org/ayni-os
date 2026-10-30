#include "fixture_json.h"
#include "normal_fixture_profile.h"
#include <algorithm>
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/host_authority.h>
#include <ayni/memory.h>
#include <ayni/resources.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/runtime/sim_node.h>
#include <ayni/storage_format.h>
#include <ayni/subscriptions.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace ayni::runtime::fixtures {
namespace {
constexpr uint32_t step_budget_us = 100000;
constexpr uint32_t fixture_goal_capacity = 32, fixture_call_capacity = 64;
constexpr uint32_t trace_limit = 20000;
constexpr int64_t initial_utc_s = 86400;
constexpr uint32_t fixture_storage_schema =
    AYNI_STORAGE_SCHEMA_CURRENT; // Actual compiled codec version.
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
ayni_scope_v1 empty_scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
// Identities, sequences and time are strings, as required by C-JSON-004.
std::string metric_scalar(const char *name, int64_t value) {
    return "," + quoted(name) + ":" + quoted(std::to_string(value));
}
std::string scalar(const char *name, uint64_t value) {
    return "," + quoted(name) + ":" + quoted(std::to_string(value));
}
ayni_config_v1 configuration(const Config &fixture) {
    auto c = default_config();
    c.storage_buffer_bytes = 0;
    c.agent_capacity = 2;
    c.goal_type_capacity = 16;
    c.goal_capacity = fixture_goal_capacity;
    c.task_capacity = fixture_goal_capacity;
    c.strategy_capacity = 2;
    c.goal_parameter_max_bytes = applications::assistant::parameter_bytes_max;
    c.task_checkpoint_max_bytes = 64;
    c.task_step_capacity = AYNI_TASK_STEP_CAPACITY_MAX;
    c.call_capacity = fixture_call_capacity;
    c.call_input_max_bytes = 640;
    c.call_result_max_bytes = 512;
    c.call_capability_capacity = 6;
    c.capability_capacity = 7;
    c.provider_capacity = 6;
    c.binding_capacity = 5;
    c.resource_capacity = 5;
    c.resource_request_capacity = 16;
    c.observation_source_capacity = 1;
    c.task_model_call_hard_limit = fixture.model_hard_limit;
    return c;
}
const char *metric_name(Metric metric) {
    switch (metric) {
    case Metric::task_state:
        return "task_state";
    case Metric::task_steps:
        return "task_steps";
    case Metric::model_calls:
        return "model_calls";
    case Metric::model_limit:
        return "model_limit";
    case Metric::hold_reason:
        return "hold_reason";
    case Metric::hold_status:
        return "hold_status";
    case Metric::goal_group_paused:
        return "goal_group_paused";
    case Metric::health_executions:
        return "health_executions";
    case Metric::goal_state:
        return "goal_state";
    case Metric::goal_count:
        return "goal_count";
    case Metric::call_count:
        return "call_count";
    case Metric::model_a_executions:
        return "model_a_executions";
    case Metric::model_b_executions:
        return "model_b_executions";
    case Metric::presentations:
        return "presentations";
    case Metric::increments:
        return "increments";
    case Metric::max_model_holders:
        return "max_model_holders";
    case Metric::max_camera_holders:
        return "max_camera_holders";
    case Metric::local_steps:
        return "local_steps";
    case Metric::subscription_stops:
        return "subscription_stops";
    case Metric::barrier_checks:
        return "barrier_checks";
    case Metric::preserved_calls:
        return "preserved_calls";
    }
    return "invalid";
}
bool matches(int64_t actual, const Expect &expect) {
    switch (expect.compare) {
    case Compare::equal:
        return actual == expect.value;
    case Compare::at_most:
        return actual <= expect.value;
    case Compare::at_least:
        return actual >= expect.value;
    }
    return false;
}
const char *comparison_name(Compare compare) {
    switch (compare) {
    case Compare::equal:
        return "equal";
    case Compare::at_most:
        return "at_most";
    case Compare::at_least:
        return "at_least";
    }
    return "invalid";
}
// Owns the entire deployment and trace until every Core borrow is destroyed.
// Trace caches are diagnostic copies, never an alternative owner of domain state.
class Engine {
  public:
    Engine(const Case &data, std::filesystem::path directory)
        : data_(data), directory_(std::move(directory)), trace_(directory_ / "trace.jsonl") {}
    Result execute() {
        auto status = open(Provider::a, Strategy::v1);
        if (status != AYNI_OK) {
            return {status, "assemble/start"};
        }
        for (uint32_t at = 0; at < data_.action_count; ++at) {
            action_ = at;
            emit("script", scalar("action", at));
            status =
                std::visit([this](const auto &value) { return apply(value); }, data_.actions[at]);
            if (status != AYNI_OK) {
                return {status, "action " + std::to_string(at)};
            }
            status = observe();
            if (status != AYNI_OK) {
                return {status, "observe after action " + std::to_string(at)};
            }
        }
        for (uint32_t at = 0; at < data_.expectation_count; ++at) {
            int64_t actual = 0;
            const auto &expected = data_.expectations[at];
            status = metric(expected, actual);
            if (status != AYNI_OK || !record_expectation(expected, actual)) {
                return {status == AYNI_OK ? AYNI_INVALID_STATE : status,
                        "expect " + std::to_string(at) + " " + metric_name(expected.metric) +
                            " actual=" + std::to_string(actual) +
                            " expected=" + std::to_string(expected.value)};
            }
        }
        return trace_.good() ? Result{} : Result{AYNI_INTERNAL, "trace output"};
    }
    ayni_status_t close() {
        auto status = AYNI_OK;
        if (node_.kernel()) {
            ayni_lifecycle_t state = 0;
            status = ayni_get_lifecycle(node_.kernel(), &state);
            if (status != AYNI_OK) {
                return status;
            }
            if (state != AYNI_LIFECYCLE_CONFIGURING && state != AYNI_LIFECYCLE_STOPPED) {
                status = ayni_begin_shutdown(node_.kernel());
                if (status != AYNI_OK) {
                    return status;
                }
                for (uint32_t at = 0; at < data_.config.max_steps; ++at) {
                    status = step();
                    if (status != AYNI_OK) {
                        return status;
                    }
                    status = ayni_get_lifecycle(node_.kernel(), &state);
                    if (status != AYNI_OK || state == AYNI_LIFECYCLE_STOPPED) {
                        break;
                    }
                }
                if (status != AYNI_OK) {
                    return status;
                }
                if (state != AYNI_LIFECYCLE_STOPPED) {
                    return AYNI_BUSY;
                }
            }
            status = node_.release();
            if (status != AYNI_OK) {
                return status;
            }
        }
        if (profile_) {
            const auto counts = profile_->totals();
            status = profile_->close();
            if (status != AYNI_OK) {
                return status;
            }
            accumulated_.health += counts.health;
            accumulated_.model_a += counts.model_a;
            accumulated_.model_b += counts.model_b;
            accumulated_.increments += counts.increments;
            accumulated_.presentations += counts.presentations;
            accumulated_.barrier_checks += counts.barrier_checks;
            profile_.reset();
        }
        return AYNI_OK;
    }
    bool manifest(const Result &result) {
        trace_.flush();
        std::ofstream out(directory_ / "run.json");
        const auto config = configuration(data_.config);
        size_t arena = 0, alignment = 0;
        const auto measured = ayni_kernel_memory_requirements(&config, &arena, &alignment);
        if (measured != AYNI_OK) {
            return false;
        }
        out << "{\"fixture_schema\":1,\"profile\":\"m1-normal\",\"case_id\":" << quoted(data_.id)
            << ",\"source\":" << quoted(data_.source)
            << ",\"revision\":" << quoted(source_revision())
            << ",\"compiler\":" << quoted(__VERSION__) << ",\"host\":\"native-sim\",\"seed\":\"0\""
            << ",\"build_type\":" << quoted(AYNI_FIXTURE_BUILD_TYPE)
            << ",\"sanitizer\":" << quoted(AYNI_FIXTURE_SANITIZER)
            << ",\"kernel_config\":" << configuration_json(config)
            << ",\"experiment_step\":" << quoted(data_.experiment_step)
            << ",\"clauses\":" << quoted(data_.clauses) << scalar("abi_major", AYNI_ABI_MAJOR)
            << scalar("abi_minor", AYNI_ABI_MINOR)
            << scalar("storage_schema", fixture_storage_schema)
            << ",\"wire_protocol\":\"not_implemented\"" << scalar("tick_us", data_.config.tick_us)
            << scalar("model_delay_us", data_.config.model_delay_us)
            << scalar("step_units", data_.config.step_units)
            << scalar("max_steps", data_.config.max_steps)
            << scalar("model_hard_limit", data_.config.model_hard_limit)
            << scalar("arena_bytes", arena) << scalar("arena_alignment", alignment)
            << ",\"status\":" << result.status << ",\"close_status\":" << result.close_status
            << ",\"artifact_status\":" << result.artifact_status
            << ",\"passed\":" << (result.status == AYNI_OK ? "true" : "false")
            << ",\"first_failure\":" << quoted(result.detail)
            << ",\"fault_recovery\":\"paused_unverified\",\"hardware\":\"unverified\"}\n";
        out.flush();
        return out.good() && trace_.good();
    }

  private:
    struct Named {
        ayni_object_kind_t kind;
        uint64_t id;
        uint64_t command = 0;
        const CreateGoal *creation = nullptr;
        std::optional<uint64_t> local_event_step{};
        std::optional<uint64_t> local_completion_steps{};
    };
    const Case &data_;
    std::filesystem::path directory_;
    std::ofstream trace_;
    // Retained by this whole Engine if close fails; Profile callbacks never borrow stack
    // temporaries.
    SimNode node_;
    std::unique_ptr<Profile> profile_;
    Totals accumulated_{};
    uint64_t command_ = 0, session_ = 0, steps_ = 0, trace_sequence_ = 0;
    uint32_t action_ = 0;
    std::map<std::string, Named> names_;
    std::map<std::string, std::string> snapshots_;
    std::set<uint64_t> stopped_subscriptions_;
    uint64_t max_model_holders_ = 0, max_camera_holders_ = 0, preserved_calls_ = 0;
    uint64_t now() const {
        const auto clock = node_.clock();
        return clock.now_us ? clock.now_us(clock.context) : 0;
    }
    void emit(const char *category, const std::string &fields) {
        if (++trace_sequence_ > trace_limit) {
            trace_.setstate(std::ios::failbit);
            return;
        }
        trace_ << "{\"category\":" << quoted(category) << scalar("sequence", trace_sequence_)
               << scalar("step", steps_) << scalar("clock_us", now()) << scalar("action", action_)
               << fields << "}\n";
    }
    void changed(const std::string &key, const char *category, const std::string &fields) {
        auto found = snapshots_.find(key);
        if (found == snapshots_.end() || found->second != fields) {
            snapshots_[key] = fields;
            emit(category, fields);
        }
    }
    bool record_expectation(const Expect &expected, int64_t actual) {
        const bool passed = matches(actual, expected);
        emit("assertion", ",\"metric\":" + quoted(metric_name(expected.metric)) +
                              ",\"target\":" + quoted(expected.target) +
                              ",\"comparison\":" + quoted(comparison_name(expected.compare)) +
                              metric_scalar("actual", actual) +
                              metric_scalar("expected", expected.value) +
                              ",\"passed\":" + (passed ? "true" : "false"));
        return passed;
    }
    ayni_status_t objects(ayni_object_kind_t kind, std::vector<uint64_t> &out) const {
        uint32_t count = 0;
        auto status = ayni_list_objects(node_.kernel(), kind, nullptr, 0, &count);
        if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
            return status;
        }
        out.resize(count);
        return ayni_list_objects(node_.kernel(), kind, out.data(), count, &count);
    }
    ayni_status_t instance(uint32_t agent, uint64_t &id) const {
        auto info = initialized<ayni_agent_info_v1>();
        const auto status = ayni_get_agent(node_.kernel(), agent, &info);
        if (status == AYNI_OK) {
            id = info.instance_id;
        }
        return status;
    }
    ayni_status_t goal(const char *name, ayni_goal_info_v1 &info) const {
        const auto found = names_.find(name);
        if (found == names_.end() || found->second.kind != AYNI_OBJECT_GOAL) {
            return AYNI_NOT_FOUND;
        }
        info = initialized<ayni_goal_info_v1>();
        return ayni_get_goal(node_.kernel(), found->second.id, &info);
    }
    ayni_status_t task(const char *name, ayni_task_info_v1 &info) const {
        ayni_goal_info_v1 owner{};
        const auto status = goal(name, owner);
        if (status != AYNI_OK) {
            return status;
        }
        if (!owner.last_task_id) {
            return AYNI_NOT_FOUND;
        }
        info = initialized<ayni_task_info_v1>();
        return ayni_get_task(node_.kernel(), owner.last_task_id, &info);
    }
    ayni_status_t open(Provider provider, Strategy strategy) {
        auto status = node_.assemble(configuration(data_.config));
        if (status != AYNI_OK) {
            return status;
        }
        if (!steps_) {
            node_.set_utc(initial_utc_s);
        }
        profile_ = std::make_unique<Profile>();
        status =
            profile_->assemble(node_.kernel(), node_.clock(), data_.config, provider, strategy);
        if (status == AYNI_OK) {
            status = ayni_start(node_.kernel());
        }
        if (status != AYNI_OK) {
            return status;
        }
        for (uint32_t at = 0; at < data_.config.max_steps; ++at) {
            status = step();
            if (status != AYNI_OK) {
                return status;
            }
            ayni_lifecycle_t state = 0;
            status = ayni_get_lifecycle(node_.kernel(), &state);
            if (status != AYNI_OK) {
                return status;
            }
            if (state == AYNI_LIFECYCLE_RUNNING) {
                const ayni_session_desc_v1 desc{sizeof(desc), AYNI_CHANNEL_MANAGEMENT, 1, 1, 0};
                status = ayni_open_session(node_.kernel(), &desc, &session_);
                // RUNNING precedes durable first-owner binding; a management session
                // becomes usable only after that original transition is confirmed.
                if (status != AYNI_NOT_READY) {
                    return status;
                }
            } else if (state != AYNI_LIFECYCLE_RECOVERING) {
                return AYNI_INVALID_STATE;
            }
        }
        return AYNI_BUSY;
    }
    ayni_status_t step() {
        node_.advance(data_.config.tick_us);
        auto status = profile_->advance();
        auto report = initialized<ayni_step_report_v1>();
        if (status == AYNI_OK) {
            status = ayni_step(node_.kernel(), data_.config.step_units, step_budget_us, &report);
        }
        ++steps_;
        if (status == AYNI_OK) {
            status = observe();
        }
        return status;
    }
    template <class Body>
    ayni_status_t submit(ayni_intent_kind_t kind, const Body &body, uint64_t command,
                         ayni_intent_result_v1 &result, uint32_t revision = 0) {
        result = initialized<ayni_intent_result_v1>();
        auto error = initialized<ayni_error_v1>();
        const ayni_intent_v1 intent{sizeof(intent), kind, command, session_, revision, 0, &body};
        const auto status = ayni_submit_intent(node_.kernel(), &intent, &result, &error);
        emit("command", scalar("command_id", command) + scalar("intent_kind", kind) +
                            scalar("object_id", status == AYNI_OK ? result.object.id : 0) +
                            scalar("commit_seq", status == AYNI_OK ? result.commit_seq : 0) +
                            ",\"status\":" + std::to_string(status));
        if (status != AYNI_OK) {
            return status;
        }
        return durable(result.commit_seq);
    }
    ayni_status_t durable(uint64_t sequence) {
        for (uint32_t at = 0; at < data_.config.max_steps; ++at) {
            auto info = initialized<ayni_kernel_info_v1>();
            auto status = ayni_get_kernel_info(node_.kernel(), &info);
            if (status != AYNI_OK || info.durable_seq >= sequence) {
                return status;
            }
            status = step();
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_BUSY;
    }
    template <class Body>
    ayni_status_t command(ayni_intent_kind_t kind, const Body &body, uint32_t revision = 0) {
        if (command_ == UINT64_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        ayni_intent_result_v1 result{};
        return submit(kind, body, ++command_, result, revision);
    }
    ayni_status_t apply(const Grant &a) {
        uint64_t actor = 0;
        auto status = instance(a.agent, actor);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_grant_create_v1 body{sizeof(body),
                                        1,
                                        text(Profile::capability(a.capability)),
                                        actor,
                                        0,
                                        AYNI_GRANT_PERMANENT,
                                        0,
                                        0,
                                        empty_scope()};
        ayni_intent_result_v1 result{};
        status = submit(AYNI_INTENT_GRANT_CREATE, body, ++command_, result);
        if (status == AYNI_OK) {
            names_.emplace(a.name, Named{AYNI_OBJECT_GRANT, result.object.id});
        }
        return status;
    }
    ayni_status_t apply(const SaveMemory &a) {
        uint64_t actor = 0;
        auto status = instance(a.agent, actor);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_memory_save_v1 body{sizeof(body),
                                       AYNI_MEMORY_PRIVATE,
                                       0,
                                       actor,
                                       AYNI_PROFILE_NONE,
                                       0,
                                       text(a.body),
                                       nullptr,
                                       0,
                                       0,
                                       nullptr};
        ayni_intent_result_v1 result{};
        status = submit(AYNI_INTENT_MEMORY_SAVE, body, ++command_, result);
        if (status == AYNI_OK) {
            names_.emplace(a.name, Named{AYNI_OBJECT_MEMORY, result.object.id});
        }
        return status;
    }
    ayni_status_t apply(const ShareMemory &a) {
        auto memory = names_.find(a.memory);
        if (memory == names_.end()) {
            return AYNI_NOT_FOUND;
        }
        uint64_t actor = 0;
        auto status = instance(a.agent, actor);
        std::array<ayni_scope_field_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
        if (status == AYNI_OK) {
            status = ayni_memory_read_scope(memory->second.id, AYNI_MEMORY_CONTEXT, fields.data(),
                                            fields.size());
        }
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_grant_create_v1 body{
            sizeof(body),
            1,
            text("memory.access"),
            actor,
            0,
            AYNI_GRANT_PERMANENT,
            0,
            0,
            {sizeof(ayni_scope_v1), static_cast<uint32_t>(fields.size()), fields.data()}};
        ayni_intent_result_v1 result{};
        status = submit(AYNI_INTENT_GRANT_CREATE, body, ++command_, result);
        if (status == AYNI_OK) {
            names_.emplace(a.name, Named{AYNI_OBJECT_GRANT, result.object.id});
        }
        return status;
    }
    ayni_status_t apply(const Revoke &a) {
        auto found = names_.find(a.grant);
        if (found == names_.end()) {
            return AYNI_NOT_FOUND;
        }
        auto info = initialized<ayni_grant_info_v1>();
        auto status = ayni_get_grant(node_.kernel(), found->second.id, &info);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_grant_revoke_v1 body{sizeof(body), 0, found->second.id};
        return command(AYNI_INTENT_GRANT_REVOKE, body, info.revision);
    }
    ayni_status_t create(const CreateGoal &a, uint64_t cmd, uint64_t &id) {
        uint64_t actor = 0;
        auto status = instance(a.agent, actor);
        if (status != AYNI_OK) {
            return status;
        }
        auto body = initialized<ayni_goal_create_v1>();
        const auto *regular = std::get_if<Goal>(&a.kind);
        const auto *assistant = std::get_if<AssistantGoal>(&a.kind);
        if (regular) {
            body.goal_type_id = text(applications::normal_fixtures::goal_type(*regular));
        }
        body.goal_type_version = 1;
        body.owner_instance_id = actor;
        body.pause_group = a.pause_group;
        body.trigger =
            (regular && *regular == Goal::local) ? AYNI_TRIGGER_LOCAL_INPUT : AYNI_TRIGGER_ONCE;
        if (regular && *regular == Goal::local) {
            body.trigger_source = text(a.name);
        }
        std::string memory;
        if ((regular && *regular == Goal::memory_read)) {
            auto found = names_.find(a.memory);
            if (found == names_.end()) {
                return AYNI_NOT_FOUND;
            }
            memory = std::to_string(found->second.id);
            body.parameters = {reinterpret_cast<const uint8_t *>(memory.data()),
                               static_cast<uint32_t>(memory.size())};
        }
        applications::assistant::Encoded encoded;
        auto calendar = initialized<ayni_calendar_rule_v1>();
        if (assistant) {
            calendar.kind = AYNI_CALENDAR_ONCE;
            calendar.rule_version = 1;
            calendar.timezone_size = 4;
            std::memcpy(calendar.timezone, "UTC0", calendar.timezone_size);
            calendar.first_local_s = a.calendar_s;
            applications::assistant::Parameters parameters;
            parameters.kind = *assistant == AssistantGoal::reminder
                                  ? applications::assistant::GoalKind::reminder
                                  : applications::assistant::GoalKind::device_check;
            parameters.text = a.body;
            status = applications::assistant::prepare_goal(
                parameters, actor, applications::assistant::check_period_us,
                *assistant == AssistantGoal::reminder ? &calendar : nullptr, encoded, body);
            if (status != AYNI_OK) {
                return status;
            }
        }
        ayni_intent_result_v1 result{};
        status = submit(AYNI_INTENT_GOAL_CREATE, body, cmd, result);
        if (status == AYNI_OK) {
            id = result.object.id;
        }
        return status;
    }
    ayni_status_t apply(const CreateGoal &a) {
        uint64_t id = 0;
        const auto cmd = ++command_;
        const auto status = create(a, cmd, id);
        if (status == AYNI_OK) {
            names_.emplace(a.name, Named{AYNI_OBJECT_GOAL, id, cmd, &a});
        }
        return status;
    }
    ayni_status_t apply(const Steps &a) {
        for (uint32_t at = 0; at < a.count; ++at) {
            const auto status = step();
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    ayni_status_t apply(const Advance &a) {
        if (a.microseconds > UINT64_MAX - now()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        node_.advance(a.microseconds);
        return AYNI_OK;
    }
    ayni_status_t apply(const WallClock &a) {
        node_.set_utc(a.seconds, a.trust);
        return AYNI_OK;
    }
    ayni_status_t apply(const Wait &a) {
        for (uint32_t at = 0; at < data_.config.max_steps; ++at) {
            int64_t actual = 0;
            auto status = metric(a.condition, actual);
            if (status == AYNI_OK && matches(actual, a.condition)) {
                return AYNI_OK;
            }
            if (status != AYNI_OK && status != AYNI_NOT_FOUND) {
                return status;
            }
            status = step();
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_BUSY;
    }
    ayni_status_t apply(const Check &a) {
        int64_t actual = 0;
        const auto status = metric(a.condition, actual);
        if (status != AYNI_OK) {
            return status;
        }
        return record_expectation(a.condition, actual) ? AYNI_OK : AYNI_INVALID_STATE;
    }
    ayni_status_t apply(const PauseGroup &a) {
        auto group = initialized<ayni_pause_group_info_v1>();
        const auto status = ayni_get_pause_group(node_.kernel(), a.group, &group);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_pause_group_set_v1 body{sizeof(body), a.group, 1, 0};
        return command(AYNI_INTENT_PAUSE_GROUP_SET, body, group.revision);
    }
    ayni_status_t apply(const ResumeGroup &a) {
        auto group = initialized<ayni_pause_group_info_v1>();
        const auto status = ayni_get_pause_group(node_.kernel(), a.group, &group);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_pause_group_set_v1 body{sizeof(body), a.group, 0, 0};
        return command(AYNI_INTENT_PAUSE_GROUP_SET, body, group.revision);
    }
    ayni_status_t apply(const PauseGoal &a) {
        auto info = initialized<ayni_goal_info_v1>();
        const auto status = goal(a.goal, info);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_goal_target_v1 body{sizeof(body), 0, info.goal_id};
        return command(AYNI_INTENT_GOAL_PAUSE, body, info.revision);
    }
    ayni_status_t apply(const ResumeGoal &a) {
        auto info = initialized<ayni_goal_info_v1>();
        const auto status = goal(a.goal, info);
        if (status != AYNI_OK) {
            return status;
        }
        const ayni_goal_target_v1 body{sizeof(body), 0, info.goal_id};
        return command(AYNI_INTENT_GOAL_RESUME, body, info.revision);
    }
    ayni_status_t apply(const LocalInput &a) {
        auto found = names_.find(a.goal);
        if (found == names_.end()) {
            return AYNI_NOT_FOUND;
        }
        auto event = initialized<ayni_event_v1>();
        event.kind = AYNI_EVENT_LOCAL_INPUT;
        if (!found->second.creation) {
            return AYNI_INVALID_ARGUMENT;
        }
        event.source = text(found->second.creation->name);
        const auto status = ayni_post_event(node_.kernel(), &event);
        if (status == AYNI_OK) {
            found->second.local_event_step = steps_;
        }
        return status;
    }
    ayni_status_t apply(const Retry &a) {
        const auto found = names_.find(a.goal);
        if (found == names_.end() || !found->second.creation) {
            return AYNI_NOT_FOUND;
        }
        uint64_t id = 0;
        const auto status = create(*found->second.creation, found->second.command, id);
        return status == AYNI_OK && id != found->second.id ? AYNI_INVALID_STATE : status;
    }
    ayni_status_t apply(const Reopen &a) {
        std::vector<uint64_t> before;
        auto status = objects(AYNI_OBJECT_CALL, before);
        if (status != AYNI_OK) {
            return status;
        }
        std::map<uint64_t, ayni_call_info_v1> retained;
        for (auto id : before) {
            auto call = initialized<ayni_call_info_v1>();
            status = ayni_get_call(node_.kernel(), id, &call);
            if (status != AYNI_OK) {
                return status;
            }
            // This normal action only replaces configuration at a quiescent
            // checkpoint. In-flight and UNKNOWN recovery remain outside this profile.
            if (call.phase != AYNI_CALL_FINISHED || !call.outcome ||
                call.effect == AYNI_CALL_EFFECT_UNKNOWN) {
                return AYNI_INVALID_STATE;
            }
            retained.emplace(id, call);
        }
        std::map<uint64_t, ayni_task_info_v1> retained_tasks;
        status = objects(AYNI_OBJECT_TASK, before);
        if (status != AYNI_OK) {
            return status;
        }
        for (auto id : before) {
            auto info = initialized<ayni_task_info_v1>();
            status = ayni_get_task(node_.kernel(), id, &info);
            if (status != AYNI_OK) {
                return status;
            }
            retained_tasks.emplace(id, info);
        }
        status = close();
        if (status == AYNI_OK) {
            status = open(a.provider, a.strategy);
        }
        if (status != AYNI_OK) {
            return status;
        }
        for (const auto &entry : retained) {
            auto call = initialized<ayni_call_info_v1>();
            status = ayni_get_call(node_.kernel(), entry.first, &call);
            const auto &old = entry.second;
            if (status != AYNI_OK || call.task_id != old.task_id ||
                call.operation_id != old.operation_id ||
                call.owner_instance_id != old.owner_instance_id || call.outcome != old.outcome ||
                call.status != old.status || call.reason != old.reason ||
                call.effect != old.effect) {
                return status == AYNI_OK ? AYNI_INVALID_STATE : status;
            }
            ++preserved_calls_;
        }
        for (const auto &entry : retained_tasks) {
            auto info = initialized<ayni_task_info_v1>();
            status = ayni_get_task(node_.kernel(), entry.first, &info);
            const auto &old = entry.second;
            if (status != AYNI_OK || info.owner_instance_id != old.owner_instance_id ||
                info.goal_id != old.goal_id || info.step_count != old.step_count ||
                info.model_calls_used != old.model_calls_used ||
                info.model_call_limit != old.model_call_limit) {
                return status == AYNI_OK ? AYNI_INVALID_STATE : status;
            }
        }
        emit("reopen_preserved",
             scalar("calls", retained.size()) + scalar("tasks", retained_tasks.size()));
        return AYNI_OK;
    }
    Totals totals() const {
        auto total = accumulated_;
        if (profile_) {
            const auto live = profile_->totals();
            total.health += live.health;
            total.model_a += live.model_a;
            total.model_b += live.model_b;
            total.increments += live.increments;
            total.presentations += live.presentations;
            total.barrier_checks += live.barrier_checks;
        }
        return total;
    }
    ayni_status_t metric(const Expect &e, int64_t &out) const {
        const auto total = totals();
        switch (e.metric) {
        case Metric::model_a_executions:
            out = static_cast<int64_t>(total.model_a);
            return AYNI_OK;
        case Metric::model_b_executions:
            out = static_cast<int64_t>(total.model_b);
            return AYNI_OK;
        case Metric::increments:
            out = static_cast<int64_t>(total.increments);
            return AYNI_OK;
        case Metric::presentations:
            out = static_cast<int64_t>(total.presentations);
            return AYNI_OK;
        case Metric::health_executions:
            out = static_cast<int64_t>(total.health);
            return AYNI_OK;
        case Metric::barrier_checks:
            out = static_cast<int64_t>(total.barrier_checks);
            return AYNI_OK;
        case Metric::max_model_holders:
            out = static_cast<int64_t>(max_model_holders_);
            return AYNI_OK;
        case Metric::max_camera_holders:
            out = static_cast<int64_t>(max_camera_holders_);
            return AYNI_OK;
        case Metric::subscription_stops:
            out = static_cast<int64_t>(stopped_subscriptions_.size());
            return AYNI_OK;
        case Metric::preserved_calls:
            out = static_cast<int64_t>(preserved_calls_);
            return AYNI_OK;
        case Metric::goal_count:
        case Metric::call_count: {
            std::vector<uint64_t> ids;
            const auto status =
                objects(e.metric == Metric::goal_count ? AYNI_OBJECT_GOAL : AYNI_OBJECT_CALL, ids);
            if (status == AYNI_OK) {
                out = static_cast<int64_t>(ids.size());
            }
            return status;
        }
        case Metric::goal_state:
        case Metric::goal_group_paused: {
            ayni_goal_info_v1 info{};
            const auto status = goal(e.target, info);
            if (status == AYNI_OK) {
                out = e.metric == Metric::goal_state ? info.lifecycle : info.group_paused;
            }
            return status;
        }
        case Metric::local_steps: {
            auto found = names_.find(e.target);
            if (found == names_.end() || !found->second.local_completion_steps) {
                return AYNI_NOT_FOUND;
            }
            out = static_cast<int64_t>(*found->second.local_completion_steps);
            return AYNI_OK;
        }
        default:
            break;
        }
        ayni_task_info_v1 info{};
        const auto status = task(e.target, info);
        if (status != AYNI_OK) {
            return status;
        }
        switch (e.metric) {
        case Metric::task_state:
            out = info.lifecycle;
            break;
        case Metric::task_steps:
            out = info.step_count;
            break;
        case Metric::model_calls:
            out = info.model_calls_used;
            break;
        case Metric::model_limit:
            out = info.model_call_limit;
            break;
        case Metric::hold_reason:
            out = info.hold_reason;
            break;
        case Metric::hold_status:
            out = info.hold_status;
            break;
        default:
            return AYNI_INVALID_ARGUMENT;
        }
        return AYNI_OK;
    }
    ayni_status_t observe();
};

ayni_status_t Engine::observe() {
    auto info = initialized<ayni_kernel_info_v1>();
    auto status = ayni_get_kernel_info(node_.kernel(), &info);
    if (status != AYNI_OK) {
        return status;
    }
    changed("kernel", "commit",
            scalar("boot_epoch", info.boot_epoch) + scalar("durable_seq", info.durable_seq) +
                scalar("state", info.lifecycle));
    if (info.lifecycle == AYNI_LIFECYCLE_RECOVERING ||
        info.lifecycle == AYNI_LIFECYCLE_CONFIGURING || info.lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return trace_.good() ? AYNI_OK : AYNI_INTERNAL;
    }
    std::vector<uint64_t> ids;
    status = objects(AYNI_OBJECT_GOAL, ids);
    if (status != AYNI_OK) {
        return status;
    }
    for (auto id : ids) {
        auto row = initialized<ayni_goal_info_v1>();
        status = ayni_get_goal(node_.kernel(), id, &row);
        if (status != AYNI_OK) {
            return status;
        }
        changed("goal." + std::to_string(id), "transition",
                scalar("goal_id", id) + scalar("transition", row.transition_id) +
                    scalar("commit_seq", row.commit_seq) + scalar("state", row.lifecycle) +
                    scalar("owner_instance_id", row.owner_instance_id) +
                    scalar("pause_group", row.pause_group) +
                    scalar("group_paused", row.group_paused));
    }
    status = objects(AYNI_OBJECT_TASK, ids);
    if (status != AYNI_OK) {
        return status;
    }
    for (auto id : ids) {
        auto row = initialized<ayni_task_info_v1>();
        status = ayni_get_task(node_.kernel(), id, &row);
        if (status != AYNI_OK) {
            return status;
        }
        changed("task." + std::to_string(id), "task_runtime",
                scalar("task_id", id) + scalar("goal_id", row.goal_id) +
                    scalar("transition", row.transition_id) + scalar("commit_seq", row.commit_seq) +
                    scalar("state", row.lifecycle) +
                    scalar("owner_instance_id", row.owner_instance_id) +
                    scalar("steps", row.step_count) + scalar("model_calls", row.model_calls_used) +
                    scalar("model_limit", row.model_call_limit) +
                    scalar("hold_reason", row.hold_reason) +
                    ",\"hold_status\":" + std::to_string(row.hold_status));
        for (auto &entry : names_) {
            auto &named = entry.second;
            if (named.kind == AYNI_OBJECT_GOAL && named.id == row.goal_id &&
                named.local_event_step && !named.local_completion_steps &&
                row.lifecycle == AYNI_TASK_COMPLETED) {
                named.local_completion_steps = steps_ - *named.local_event_step;
            }
        }
    }
    status = objects(AYNI_OBJECT_CALL, ids);
    if (status != AYNI_OK) {
        return status;
    }
    for (auto id : ids) {
        auto row = initialized<ayni_call_info_v1>();
        status = ayni_get_call(node_.kernel(), id, &row);
        if (status != AYNI_OK) {
            return status;
        }
        changed("call." + std::to_string(id), "call",
                scalar("logical_call_id", id) + scalar("task_id", row.task_id) +
                    scalar("operation_id", row.operation_id) +
                    scalar("boot_epoch", row.attempt.boot_epoch) +
                    scalar("request_id", row.attempt.request_id) +
                    scalar("requires_seq", row.requires_seq) + scalar("phase", row.phase) +
                    scalar("outcome", row.outcome) + scalar("effect", row.effect) +
                    scalar("wait_reason", row.wait_reason));
        auto sub = initialized<ayni_subscription_info_v1>();
        status = ayni_get_subscription(node_.kernel(), id, &sub);
        if (status == AYNI_OK) {
            changed("subscription." + std::to_string(id), "subscription",
                    scalar("logical_call_id", id) + scalar("phase", sub.phase) +
                        scalar("generation", sub.subscription.generation) +
                        scalar("boot_epoch", sub.subscription.boot_epoch) +
                        scalar("lease_sequence", sub.lease_sequence));
            if (sub.phase == AYNI_SUBSCRIPTION_STOPPED) {
                stopped_subscriptions_.insert(id);
            }
        } else if (status != AYNI_NOT_FOUND) {
            return status;
        }
    }
    for (const char *resource :
         {"model.slot", "camera.slot", "counter.slot", "notification.slot", "device.slot"}) {
        auto row = initialized<ayni_resource_info_v1>();
        status = ayni_get_resource(node_.kernel(), text(resource), &row);
        if (status != AYNI_OK) {
            return status;
        }
        if (row.used > row.capacity) {
            return AYNI_INVALID_STATE;
        }
        if (std::strcmp(resource, "model.slot") == 0) {
            max_model_holders_ = std::max(max_model_holders_, uint64_t{row.used});
        }
        if (std::strcmp(resource, "camera.slot") == 0) {
            max_camera_holders_ = std::max(max_camera_holders_, uint64_t{row.used});
        }
        changed(resource, "resource",
                ",\"resource\":" + quoted(resource) + scalar("used", row.used) +
                    scalar("capacity", row.capacity) + scalar("state", row.state));
    }
    const auto total = totals();
    changed("effects", "side_effect",
            scalar("model_a", total.model_a) + scalar("model_b", total.model_b) +
                scalar("increments", total.increments) +
                scalar("presentations", total.presentations) +
                scalar("barrier_checks", total.barrier_checks) + scalar("health", total.health));
    return trace_.good() ? AYNI_OK : AYNI_INTERNAL;
}
} // namespace

Result run(const Case &data, const char *output_directory) {
    if (!output_directory || !*output_directory) {
        return {AYNI_INVALID_ARGUMENT, "missing output directory"};
    }
    const auto table = cases();
    bool registered = false;
    for (size_t at = 0; at < table.size; ++at) {
        if (&table.data[at] == &data) {
            registered = true;
            break;
        }
    }
    if (!registered) {
        return {AYNI_INVALID_ARGUMENT, "unregistered fixture data"};
    }
    std::unique_ptr<Engine> engine;
    Result result;
    // If any allocation/stream exception escapes cleanup, keep the complete
    // composition. A child Provider destructor alone cannot retain policy contexts.
    struct RetainUntilClosed {
        std::unique_ptr<Engine> &engine;
        bool closed = false;
        ~RetainUntilClosed() {
            if (engine && !closed) {
                static_cast<void>(engine.release());
            }
        }
    } guard{engine};
    try {
        const std::filesystem::path directory(output_directory);
        std::error_code error;
        if (!directory.parent_path().empty()) {
            std::filesystem::create_directories(directory.parent_path(), error);
        }
        if (error || !std::filesystem::create_directory(directory, error)) {
            return {AYNI_INVALID_ARGUMENT, "output directory must be new and writable"};
        }
        engine = std::make_unique<Engine>(data, directory);
        result = engine->execute();
    } catch (const std::bad_alloc &) {
        result.status = AYNI_CAPACITY_EXCEEDED;
        result.detail.clear(); // Error conversion itself must not allocate.
    } catch (const std::exception &) {
        result.status = AYNI_INTERNAL;
        result.detail.clear();
    }
    if (engine) {
        try {
            result.close_status = engine->close();
        } catch (const std::bad_alloc &) {
            result.close_status = AYNI_CAPACITY_EXCEEDED;
        } catch (const std::exception &) {
            result.close_status = AYNI_INTERNAL;
        }
        guard.closed = result.close_status == AYNI_OK;
        if (!guard.closed && result.status == AYNI_OK) {
            result.status = result.close_status;
        }
        try {
            if (!engine->manifest(result)) {
                result.artifact_status = AYNI_INTERNAL;
            }
        } catch (const std::bad_alloc &) {
            result.artifact_status = AYNI_CAPACITY_EXCEEDED;
        } catch (const std::exception &) {
            result.artifact_status = AYNI_INTERNAL;
        }
        if (result.status == AYNI_OK) {
            result.status = result.artifact_status;
        }
    }
    return result;
}
} // namespace ayni::runtime::fixtures
