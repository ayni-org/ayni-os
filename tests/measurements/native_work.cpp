// F18: normal public-API population, then read-only white-box timing of the
// actual journal encoder. No injected stamps, recovery faults or private writes.
#include "kernel_config_fixture.h"
#include "kernel_test_probe.h"
#include "measurement_metadata.h"
#include "measurement_profiles.h"

#include <ayni/host_authority.h>
#include <ayni/hosts/sim.h>
#include <ayni/memory.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Probe = ayni::kernel::KernelTestProbe;
constexpr uint64_t owner_id = 1;
constexpr uint64_t agent_id = 1;
constexpr uint32_t schema_version = 1;
constexpr uint32_t max_owner_steps = 100000;
constexpr uint32_t step_items = 1;
constexpr uint32_t step_budget_us = 1000;
constexpr uint32_t storage_interval_us = 1;
constexpr uint64_t host_storage_capacity_bytes = UINT64_C(1073741824);
constexpr uint32_t encoding_samples = 1000;
constexpr uint32_t quiet_samples = 1000;
constexpr uint32_t small_goals = 16;
constexpr uint32_t small_tasks = 8;
constexpr uint32_t small_grants = 8;
constexpr uint32_t small_memories = 8;
constexpr uint32_t percentile_denominator = 100;
constexpr uint32_t p99_percentile = 99;
constexpr uint8_t synthetic_byte = 'x';

ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
uint64_t now_us(void *) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
}
uint64_t elapsed_ns(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
void require(bool ok, const char *operation) {
    if (!ok) {
        throw std::runtime_error(operation);
    }
}
void check(ayni_status_t status, const char *operation) {
    if (status != AYNI_OK) {
        throw std::runtime_error(std::string(operation) + ": status=" + std::to_string(status));
    }
}

// Observed samples, never a proven worst-case execution bound. p99 uses nearest
// rank, so it remains well-defined for a short smoke workload as well.
struct Samples {
    std::vector<uint64_t> values;
    void write(std::ostream &out) const {
        require(!values.empty(), "empty samples");
        auto sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const auto rank =
            (sorted.size() * p99_percentile + percentile_denominator - 1) / percentile_denominator;
        out << "{\"count\":" << sorted.size() << ",\"max_ns\":" << sorted.back()
            << ",\"p99_ns\":" << sorted[rank - 1] << '}';
    }
};

// Owns every Kernel borrow at a stable address. Failed teardown retains the
// entire state; no Host or caller arena is freed while a callback may use it.
struct State {
    ayni::hosts::sim::Host host;
    ayni_kernel_t *kernel = nullptr;
    void *arena = nullptr;
    size_t arena_bytes = 0;
    size_t alignment = 0;
    ~State() {
        if (arena != nullptr) {
            ::operator delete(arena, std::align_val_t{alignment});
        }
    }
};

class Measurement {
  public:
    explicit Measurement(bool small) : state_(std::make_unique<State>()), small_(small) {
        config_ = ayni_measure_maximum_config();
        if (small) {
            config_.goal_capacity = small_goals;
            config_.task_capacity = small_tasks;
            config_.grant_capacity = small_grants;
            config_.memory_capacity = small_memories;
            // Keep the smoke allocation small; the manual workload keeps all maxima.
            config_.command_capacity = small_goals;
            config_.call_capacity = 1;
            config_.agent_capacity = 1;
        }
        config_.storage_write_interval_us = storage_interval_us;
        // Maximum capacities reserve room for the largest possible snapshot,
        // even though this workload fills only selected tables. This is a
        // logical Host storage limit, not a preallocated 1 GiB RAM buffer.
        state_->host.set_storage_capacity(host_storage_capacity_bytes);
    }
    ~Measurement() {
        if (!close()) {
            std::cerr << "Kernel teardown incomplete; retained measurement state\n";
            static_cast<void>(state_.release());
        }
    }
    void run(std::ostream &out) {
        assemble();
        register_catalog();
        check(ayni_start(kernel()), "start");
        settle();
        const ayni_session_desc_v1 session{sizeof(session), AYNI_CHANNEL_MANAGEMENT, owner_id, 1,
                                           0};
        check(ayni_open_session(kernel(), &session, &session_), "session");
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        check(ayni_get_agent(kernel(), agent_id, &agent), "agent");
        instance_ = agent.instance_id;
        const auto capacities = actual_capacities();
        batch_bound_ = ayni::kernel::transition_batch_max(capacities);
        storage_min_ = ayni::kernel::storage_buffer_min(capacities);
        snapshot_record_bound_ = ayni::kernel::snapshot_record_max(capacities);
        snapshot_payload_bound_ = ayni::kernel::snapshot_payload_max(capacities);
        measure_buffer_requirement();
        populate();
        for (uint32_t index = 0; index < quiet_samples; ++index) {
            step(quiet_steps_);
        }
        measure_full_work();
        const ayni_instance_target_v1 stop{sizeof(stop), 0, instance_};
        const auto stopped = submit(AYNI_INTENT_INSTANCE_STOP, stop, agent_info().revision);
        stop_seq_ = stopped.commit_seq;
        // This legal bulk transition records all affected work; compare its
        // standalone encoding before later owner work changes its records.
        measure_encoding(&bulk_encoding_, &bulk_encoded_bytes_);
        settle();
        require(agent_info().lifecycle == AYNI_AGENT_STOPPED, "instance did not stop");
        check(ayni_begin_shutdown(kernel()), "shutdown");
        settle();
        require(info().lifecycle == AYNI_LIFECYCLE_STOPPED, "shutdown not stopped");
        check(ayni_destroy(kernel()), "destroy");
        state_->kernel = nullptr;
        write(out);
    }

  private:
    ayni_kernel_t *kernel() const { return state_->kernel; }
    ayni_kernel_info_v1 info() const {
        ayni_kernel_info_v1 result{};
        result.struct_size = sizeof(result);
        check(ayni_get_kernel_info(kernel(), &result), "kernel info");
        return result;
    }
    ayni_agent_info_v1 agent_info() const {
        ayni_agent_info_v1 result{};
        result.struct_size = sizeof(result);
        check(ayni_get_agent(kernel(), agent_id, &result), "agent info");
        return result;
    }
    void assemble() {
        check(ayni_kernel_memory_requirements(&config_, &state_->arena_bytes, &state_->alignment),
              "memory requirements");
        state_->arena = ::operator new(state_->arena_bytes, std::align_val_t{state_->alignment});
        auto ports = state_->host.table();
        ports.monotonic_now_us = now_us;
        check(ayni_create(&config_, &ports, state_->arena, state_->arena_bytes, &state_->kernel),
              "create");
    }
    // Policies only populate bounded local step/checkpoint records. The second
    // decision returns UNSUPPORTED, leaving a live paused Task for the bulk stop.
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *task,
                                ayni_policy_output_v1 *output) {
        if (task->step_count != 0) {
            return AYNI_UNSUPPORTED;
        }
        output->intent_count = task->step_capacity;
        for (uint32_t index = 0; index < task->step_capacity; ++index) {
            auto &intent = output->intents[index];
            intent = {};
            intent.struct_size = sizeof(intent);
            intent.kind = AYNI_POLICY_REPORT_PROGRESS;
            intent.expected_revision = task->revision;
            const auto key = std::string("step.") + std::to_string(index);
            intent.step_key_size = static_cast<uint32_t>(key.size());
            std::memcpy(intent.step_key, key.data(), key.size());
        }
        std::memset(output->checkpoint, synthetic_byte, output->checkpoint_capacity);
        output->checkpoint_size = output->checkpoint_capacity;
        return AYNI_OK;
    }
    static ayni_status_t validate(void *, ayni_bytes_v1) { return AYNI_OK; }
    void register_catalog() {
        const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, owner_id, 1, 0};
        check(ayni_register_principal(kernel(), &owner), "register owner");
        const ayni_agent_desc_v1 agent{sizeof(agent), 0, agent_id};
        check(ayni_register_agent(kernel(), &agent), "register agent");
        ayni_strategy_v1 strategy{};
        strategy.struct_size = sizeof(strategy);
        strategy.version = 1;
        strategy.strategy_id = text("measure.local");
        strategy.decide = decide;
        strategy.max_intents_per_call = config_.task_step_capacity;
        strategy.intent_size = sizeof(ayni_policy_intent_v1);
        strategy.max_checkpoint_bytes = config_.task_checkpoint_max_bytes;
        check(ayni_register_strategy(kernel(), &strategy), "register strategy");
        ayni_goal_type_v1 type{};
        type.struct_size = sizeof(type);
        type.version = 1;
        type.goal_type_id = text("measure.work");
        type.validate = validate;
        type.strategy_id = text("measure.local");
        type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) |
                             AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) |
                             AYNI_TRIGGER_BIT(AYNI_TRIGGER_LOCAL_INPUT);
        type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
        check(ayni_register_goal_type(kernel(), &type), "register type");
        const ayni_capability_desc_v1 capability{
            sizeof(capability), 1, text("measure.access"), 0, 0, nullptr};
        check(ayni_register_capability(kernel(), &capability), "register capability");
    }
    template <class Body>
    ayni_intent_result_v1 submit(ayni_intent_kind_t kind, const Body &body, uint32_t revision = 0) {
        const ayni_intent_v1 intent{sizeof(intent), kind, ++command_, session_, revision, 0, &body};
        ayni_intent_result_v1 result{};
        result.struct_size = sizeof(result);
        ayni_error_v1 error{};
        error.struct_size = sizeof(error);
        const auto start = Clock::now();
        const auto status = ayni_submit_intent(kernel(), &intent, &result, &error);
        submit_times_.values.push_back(elapsed_ns(start));
        if (status != AYNI_OK) {
            throw std::runtime_error(
                "submit kind=" + std::to_string(kind) + " command=" + std::to_string(command_) +
                " reason=" + std::to_string(error.reason) + " status=" + std::to_string(status));
        }
        return result;
    }
    ayni_step_report_v1 step(Samples &samples) {
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        const auto start = Clock::now();
        const auto status = ayni_step(kernel(), step_items, step_budget_us, &report);
        samples.values.push_back(elapsed_ns(start));
        check(status, "step");
        require(report.items_done <= step_items, "step item budget");
        return report;
    }
    // Normal owner progress only. Real monotonic time enforces Core's time
    // budget; asynchronous in-memory Host completions occur outside step timing.
    void settle() { settle(population_and_drain_); }
    void settle(Samples &samples) {
        for (uint32_t round = 0; round < max_owner_steps; ++round) {
            state_->host.complete_storage();
            const auto report = step(samples);
            if (&samples == &full_work_steps_) {
                full_work_items_ += report.items_done;
            }
            const auto status = info();
            require(status.lifecycle != AYNI_LIFECYCLE_RESTRICTED &&
                        status.lifecycle != AYNI_LIFECYCLE_FENCED,
                    "restricted lifecycle");
            if (status.lifecycle == AYNI_LIFECYCLE_STOPPED ||
                (status.lifecycle == AYNI_LIFECYCLE_RUNNING &&
                 state_->host.pending_storage() == 0 && Probe::log(kernel()).idle() &&
                 status.durable_seq == Probe::work(kernel()).transitions.latest().commit_seq &&
                 report.more_ready == 0 && all_tasks_paused())) {
                return;
            }
        }
        throw std::runtime_error("owner progress limit");
    }
    bool all_tasks_paused() const {
        // White-box observation only; public intents/strategies own all mutations.
        const auto &model = Probe::work(kernel());
        for (uint32_t index = 0; index < model.tasks.size(); ++index) {
            const auto state = model.tasks.at(index).lifecycle;
            if (state != AYNI_TASK_PAUSED && ayni::work::TaskTable::is_live(state)) {
                return false;
            }
        }
        return true;
    }
    void populate() {
        const std::vector<uint8_t> parameters(config_.goal_parameter_max_bytes, synthetic_byte);
        for (uint32_t index = 0; index < config_.goal_capacity; ++index) {
            ayni_goal_create_v1 body{};
            body.struct_size = sizeof(body);
            body.goal_type_version = 1;
            body.goal_type_id = text("measure.work");
            body.owner_instance_id = instance_;
            body.parameters = {parameters.data(), static_cast<uint32_t>(parameters.size())};
            body.trigger = index < config_.task_capacity ? AYNI_TRIGGER_ONCE : AYNI_TRIGGER_NONE;
            if (index + 1 == config_.goal_capacity) {
                body.trigger = AYNI_TRIGGER_LOCAL_INPUT;
                body.trigger_source = text("measure.input");
            }
            submit(AYNI_INTENT_GOAL_CREATE, body);
            settle();
        }
        const ayni_scope_v1 full{sizeof(full), 0, nullptr};
        for (uint32_t index = 0; index < config_.grant_capacity; ++index) {
            const ayni_grant_create_v1 body{
                sizeof(body), 1, text("measure.access"), instance_, 0, AYNI_GRANT_PERMANENT, 0, 0,
                full};
            submit(AYNI_INTENT_GRANT_CREATE, body);
            settle();
        }
        const std::string body(config_.memory_body_max_bytes, static_cast<char>(synthetic_byte));
        for (uint32_t index = 0; index < config_.memory_capacity; ++index) {
            const ayni_memory_save_v1 value{sizeof(value),
                                            AYNI_MEMORY_PRIVATE,
                                            0,
                                            instance_,
                                            AYNI_PROFILE_NONE,
                                            0,
                                            {body.data(), static_cast<uint32_t>(body.size())},
                                            nullptr,
                                            0,
                                            0,
                                            nullptr};
            const auto saved = submit(AYNI_INTENT_MEMORY_SAVE, value);
            if (index + 1 == config_.memory_capacity) {
                single_seq_ = saved.commit_seq;
                measure_encoding();
            }
            settle();
        }
        const auto &model = Probe::work(kernel());
        require(model.goals.size() == config_.goal_capacity, "Goal population");
        require(model.tasks.size() == config_.task_capacity, "Task population");
        for (uint32_t index = 0; index < model.tasks.size(); ++index) {
            const auto &task = model.tasks.at(index);
            require(task.lifecycle == AYNI_TASK_PAUSED &&
                        task.step_count == config_.task_step_capacity &&
                        model.tasks.checkpoint(index).size == config_.task_checkpoint_max_bytes,
                    "Task retained content population");
        }
        require(model.memories.size() == config_.memory_capacity, "memory population");
        require(model.grants.size() == config_.grant_capacity, "Grant population");
    }
    ayni::kernel::KernelCapacities actual_capacities() const {
        return ayni::test::capacities_of(config_);
    }
    // Retire one paused Task, then deliver a normal input to the pre-existing
    // final Goal. All four table sizes stay at capacity while step derives and
    // decides a replacement Task with full steps/checkpoint, then pauses it.
    void measure_full_work() {
        const auto old_task_id = Probe::work(kernel()).tasks.at(0).task_id;
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        check(ayni_get_task(kernel(), old_task_id, &task), "retire Task lookup");
        const ayni_task_target_v1 cancel{sizeof(cancel), 0, old_task_id};
        submit(AYNI_INTENT_TASK_CANCEL, cancel, task.revision);
        settle();
        ayni_event_v1 event{};
        event.struct_size = sizeof(event);
        event.kind = AYNI_EVENT_LOCAL_INPUT;
        event.source = text("measure.input");
        check(ayni_post_event(kernel(), &event), "full-table local input");
        settle(full_work_steps_);
        const auto &model = Probe::work(kernel());
        bool replacement = false;
        for (uint32_t index = 0; index < model.tasks.size(); ++index) {
            const auto &row = model.tasks.at(index);
            if (row.goal_id == model.goals.at(model.goals.size() - 1).goal_id) {
                require(row.task_id != old_task_id && row.lifecycle == AYNI_TASK_PAUSED &&
                            row.step_count == config_.task_step_capacity &&
                            model.tasks.checkpoint(index).size == config_.task_checkpoint_max_bytes,
                        "full-table replacement Task");
                replacement = true;
            }
        }
        require(replacement && full_work_items_ > 0, "full-table work did not execute");
        require(model.goals.size() == config_.goal_capacity &&
                    model.tasks.size() == config_.task_capacity &&
                    model.grants.size() == config_.grant_capacity &&
                    model.memories.size() == config_.memory_capacity,
                "full-table work occupancy");
    }
    // Query the public admission boundary instead of copying Core's buffer
    // selection formula. Other configuration fields stay identical.
    void measure_buffer_requirement() {
        uint32_t lower = AYNI_STORAGE_BUFFER_BYTES_MIN;
        uint32_t upper = AYNI_STORAGE_BUFFER_BYTES_MAX;
        auto explicit_config = config_;
        while (lower < upper) {
            const auto candidate = lower + (upper - lower) / 2;
            explicit_config.storage_buffer_bytes = candidate;
            size_t bytes = 0, alignment = 0;
            const auto status =
                ayni_kernel_memory_requirements(&explicit_config, &bytes, &alignment);
            require(status == AYNI_OK || status == AYNI_INVALID_ARGUMENT, "buffer admission query");
            if (status == AYNI_OK) {
                upper = candidate;
            } else {
                lower = candidate + 1;
            }
        }
        explicit_config.storage_buffer_bytes = lower;
        size_t bytes = 0, alignment = 0;
        check(ayni_kernel_memory_requirements(&explicit_config, &bytes, &alignment),
              "minimum explicit buffer");
        accepted_buffer_min_ = lower;
    }
    void measure_encoding(Samples *samples = nullptr, size_t *bytes = nullptr) {
        if (samples == nullptr) {
            samples = &encoding_;
            bytes = &encoded_bytes_;
        }
        const auto &model = Probe::work(kernel());
        const auto stamp = model.transitions.latest();
        require(stamp.commit_seq == (samples == &encoding_ ? single_seq_ : stop_seq_),
                "measurement transition identity");
        std::vector<uint8_t> buffer(batch_bound_);
        for (uint32_t index = 0; index < encoding_samples; ++index) {
            const auto start = Clock::now();
            ayni::durability::BatchWriter writer(
                buffer.data(), buffer.size(),
                {state_->host.writer_epoch(), stamp.commit_seq, stamp.transition_id});
            const auto size = ayni::kernel::encode_transition(model, stamp, writer);
            samples->values.push_back(elapsed_ns(start));
            require(size != 0 && size <= batch_bound_, "encoding bounds");
            require(index == 0 || *bytes == size, "encoding sample size changed");
            *bytes = size;
        }
        ayni::durability::BatchView view{};
        require(ayni::durability::read_batch(buffer.data(), *bytes, view) ==
                    ayni::durability::BatchCheck::ok,
                "encoded batch validation");
    }
    bool close() noexcept {
        if (kernel() == nullptr) {
            return true;
        }
        try {
            const auto status = info();
            if (status.lifecycle != AYNI_LIFECYCLE_CONFIGURING &&
                status.lifecycle != AYNI_LIFECYCLE_STOPPED) {
                check(ayni_begin_shutdown(kernel()), "cleanup shutdown");
                settle();
            }
            if (state_->host.pending_storage() != 0 || ayni_destroy(kernel()) != AYNI_OK) {
                return false;
            }
            state_->kernel = nullptr;
            return true;
        } catch (...) {
            return false;
        }
    }
    void write(std::ostream &out) const {
        out << "{\"schema\":" << schema_version << ",\"profile\":\""
            << (small_ ? "small" : "maximum") << "\",\"build_type\":\"" << AYNI_MEASURE_BUILD_TYPE
            << "\",\"sanitizer\":\"" << AYNI_MEASURE_SANITIZER << "\",\"compiler\":\""
            << __VERSION__
            << "\",\"clock\":\"steady_clock\",\"arena_bytes\":" << state_->arena_bytes
            << ",\"arena_alignment\":" << state_->alignment << ",\"source_revision\":\""
            << AYNI_MEASURE_REVISION << "\",\"source_dirty\":" << AYNI_MEASURE_DIRTY
            << ",\"host_storage_capacity_bytes\":" << host_storage_capacity_bytes
            << ",\"snapshot_record_bound_bytes\":" << snapshot_record_bound_
            << ",\"snapshot_payload_bound_bytes\":" << snapshot_payload_bound_
            << ",\"storage_min_bytes\":" << storage_min_
            << ",\"accepted_buffer_min_bytes\":" << accepted_buffer_min_
            << ",\"batch_bound_bytes\":" << batch_bound_
            << ",\"single_encoded_bytes\":" << encoded_bytes_
            << ",\"bulk_encoded_bytes\":" << bulk_encoded_bytes_
            << ",\"goals\":" << config_.goal_capacity << ",\"tasks\":" << config_.task_capacity
            << ",\"grants\":" << config_.grant_capacity
            << ",\"memories\":" << config_.memory_capacity
            << ",\"calls\":0,\"step_capacity\":" << config_.task_step_capacity
            << ",\"checkpoint_bytes\":" << config_.task_checkpoint_max_bytes
            << ",\"goal_parameter_bytes\":" << config_.goal_parameter_max_bytes
            << ",\"memory_body_bytes\":" << config_.memory_body_max_bytes
            << ",\"step_max_items\":" << step_items << ",\"step_max_time_us\":" << step_budget_us
            << ",\"storage_write_interval_us\":" << storage_interval_us << ",\"stop_commit_seq\":\""
            << stop_seq_ << "\",\"submit\":";
        submit_times_.write(out);
        out << ",\"encoding_single\":";
        encoding_.write(out);
        out << ",\"encoding_bulk\":";
        bulk_encoding_.write(out);
        out << ",\"step_population_and_drain\":";
        population_and_drain_.write(out);
        out << ",\"full_work_items\":" << full_work_items_ << ",\"step_full_work\":";
        full_work_steps_.write(out);
        out << ",\"step_quiet\":";
        quiet_steps_.write(out);
        out << ",\"close_status\":0}\n";
    }
    std::unique_ptr<State> state_;
    ayni_config_v1 config_{};
    bool small_;
    uint64_t session_ = 0, instance_ = 0, command_ = 0, stop_seq_ = 0, single_seq_ = 0;
    uint32_t batch_bound_ = 0, storage_min_ = 0, snapshot_record_bound_ = 0,
             accepted_buffer_min_ = 0;
    uint64_t snapshot_payload_bound_ = 0, full_work_items_ = 0;
    size_t encoded_bytes_ = 0, bulk_encoded_bytes_ = 0;
    Samples submit_times_, encoding_, bulk_encoding_, population_and_drain_, full_work_steps_,
        quiet_steps_;
};
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--build-info") {
        std::cout << "{\"source_revision\":\"" << AYNI_MEASURE_REVISION
                  << "\",\"source_dirty\":" << AYNI_MEASURE_DIRTY << ",\"build_type\":\""
                  << AYNI_MEASURE_BUILD_TYPE << "\",\"sanitizer\":\"" << AYNI_MEASURE_SANITIZER
                  << "\",\"compiler\":\"" << __VERSION__ << "\"}\n";
        return std::cout.good() ? 0 : 1;
    }
    if (argc != 3 || (std::string(argv[1]) != "--small" && std::string(argv[1]) != "--maximum")) {
        std::cerr << "usage: ayni_native_measurements --small|--maximum OUTPUT.json\n";
        return 2;
    }
    try {
        std::ofstream output(argv[2], std::ios::out | std::ios::trunc);
        require(output.good(), "open measurement output");
        Measurement measurement(std::string(argv[1]) == "--small");
        measurement.run(output);
        output.close();
        require(output.good(), "write measurement output");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
