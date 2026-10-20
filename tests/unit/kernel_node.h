// One simulation Host and one kernel in memory the test owns, shared by the
// tests that drive a kernel through the C ABI. The destructor drives the
// kernel to STOPPED and destroys it, so every test leaves no slot in use.

#ifndef AYNI_TESTS_UNIT_KERNEL_NODE_H
#define AYNI_TESTS_UNIT_KERNEL_NODE_H

#include "kernel/kernel_config.h"
#include "kernel/work_journal.h"

#include <ayni/ayni.h>
#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ayni::test {

constexpr uint32_t unbounded_items = 1000;
constexpr uint32_t unbounded_time_us = 1000000;
// Holds every batch a test commits, so that the batches a test never stores
// wait without room running out.
constexpr uint32_t test_storage_buffer_bytes = 65536;

// White-box: the capacities ayni_create reads from config, which covers
// every field.
inline ayni::kernel::KernelCapacities capacities_of(const ayni_config_v1 &config) {
    return {config.ingress_capacity,      config.event_body_max_bytes,
            config.completion_slot_count, config.storage_buffer_bytes,
            config.agent_capacity,        config.goal_type_capacity,
            config.goal_capacity,         config.goal_parameter_max_bytes,
            config.session_capacity,      config.task_capacity,
            config.strategy_capacity,     config.policy_intent_capacity,
            config.task_step_capacity,    config.task_checkpoint_max_bytes,
            config.command_capacity};
}

// White-box: the smallest storage buffer ayni_create accepts with config's
// other capacities, which holds the largest batch a transition writes.
inline uint32_t smallest_storage_buffer(const ayni_config_v1 &config) {
    return ayni::kernel::storage_buffer_min(capacities_of(config));
}

class Node {
  public:
    // Small capacities; a test may change config() before create().
    explicit Node(uint32_t storage_buffer_bytes = test_storage_buffer_bytes,
                  bool with_storage = true) {
        config_.struct_size = sizeof(config_);
        config_.abi_major = AYNI_ABI_MAJOR;
        config_.abi_minor = AYNI_ABI_MINOR;
        config_.ingress_capacity = 2;
        config_.event_body_max_bytes = 8;
        config_.completion_slot_count = 4;
        config_.storage_buffer_bytes = storage_buffer_bytes;
        config_.agent_capacity = 2;
        config_.goal_type_capacity = 2;
        config_.goal_capacity = 8;
        config_.goal_parameter_max_bytes = 16;
        config_.session_capacity = 2;
        config_.goal_depth_max = 2;
        config_.goal_children_max = 2;
        config_.task_capacity = 4;
        config_.strategy_capacity = 1;
        config_.policy_intent_capacity = 2;
        config_.task_step_capacity = 4;
        config_.task_checkpoint_max_bytes = 8;
        config_.command_capacity = 4;
        table_ = sim_.table();
        if (!with_storage) {
            table_.storage_append = nullptr;
            table_.storage_read_prefix = nullptr;
        }
    }

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    ~Node() { EXPECT_EQ(stop_and_destroy(), AYNI_OK); }

    ayni::hosts::sim::Host &sim() { return sim_; }
    ayni_kernel_t *kernel() { return kernel_; }
    ayni_config_v1 &config() { return config_; }

    // Before create(): reads go to port instead of the sim log.
    void replace_read_port(decltype(ayni_host_v1::storage_read_prefix) port) {
        table_.storage_read_prefix = port;
    }

    // Before create(): the kernel reads the clock through port, whose ctx is
    // the sim Host.
    void replace_clock_port(decltype(ayni_host_v1::monotonic_now_us) port) {
        table_.monotonic_now_us = port;
    }

    // Called on each new kernel right after ayni_create, so that a reboot
    // registers the same catalog again.
    void on_create(std::function<void(ayni_kernel_t *)> registration) {
        registration_ = std::move(registration);
    }

    void create() {
        std::size_t size = 0;
        std::size_t align = 0;
        ASSERT_EQ(ayni_kernel_memory_requirements(&config_, &size, &align), AYNI_OK);
        ASSERT_LE(align, alignof(uint64_t));
        memory_.resize(size / sizeof(uint64_t) + 1U);
        ASSERT_EQ(ayni_create(&config_, &table_, memory_.data(), size, &kernel_), AYNI_OK);
        if (registration_) {
            registration_(kernel_);
        }
    }

    void create_and_start() {
        create();
        ASSERT_EQ(ayni_start(kernel_), AYNI_OK);
    }

    ayni_step_report_v1 step(uint32_t max_items = unbounded_items,
                             uint32_t max_time_us = unbounded_time_us) {
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        EXPECT_EQ(ayni_step(kernel_, max_items, max_time_us, &report), AYNI_OK);
        return report;
    }

    // Steps and completes storage until recovery has an outcome.
    void recover() {
        for (int round = 0; round < 100 && lifecycle() == AYNI_LIFECYCLE_RECOVERING; ++round) {
            step();
            sim_.complete_storage();
        }
        EXPECT_NE(lifecycle(), AYNI_LIFECYCLE_RECOVERING);
        // Stores the batch that started the work model, so that a test in
        // RUNNING begins with nothing in flight.
        for (int round = 0; round < 100 && lifecycle() == AYNI_LIFECYCLE_RUNNING; ++round) {
            const ayni_step_report_v1 report = step();
            if (sim_.complete_storage() == 0U && report.more_ready == 0U) {
                break;
            }
        }
    }

    // create_and_start() and recover() into RUNNING.
    void run() {
        create_and_start();
        recover();
        ASSERT_EQ(lifecycle(), AYNI_LIFECYCLE_RUNNING);
    }

    // Builds a fresh kernel in the given state.
    void enter(ayni_lifecycle_t state) {
        switch (state) {
        case AYNI_LIFECYCLE_CONFIGURING:
            create();
            break;
        case AYNI_LIFECYCLE_RECOVERING:
            create_and_start();
            step(); // a read in flight
            break;
        case AYNI_LIFECYCLE_RUNNING:
            create_and_start();
            recover();
            break;
        case AYNI_LIFECYCLE_RESTRICTED:
            sim_.inject_fault(ayni::hosts::sim::StoragePort::read_prefix,
                              ayni::hosts::sim::StorageFault::reject);
            create_and_start();
            recover();
            break;
        case AYNI_LIFECYCLE_DRAINING:
            create_and_start();
            step();
            ASSERT_EQ(ayni_begin_shutdown(kernel_), AYNI_OK);
            break;
        case AYNI_LIFECYCLE_STOPPED:
            create_and_start();
            recover();
            ASSERT_EQ(ayni_begin_shutdown(kernel_), AYNI_OK);
            step();
            break;
        default:
            FAIL() << "no such state " << state;
        }
        ASSERT_EQ(lifecycle(), state);
    }

    ayni_lifecycle_t lifecycle() const {
        ayni_lifecycle_t state = 0;
        EXPECT_EQ(ayni_get_lifecycle(kernel_, &state), AYNI_OK);
        return state;
    }

    ayni_kernel_info_v1 info() const {
        ayni_kernel_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_kernel_info(kernel_, &out), AYNI_OK);
        return out;
    }

    // ayni_destroy; the node forgets the kernel once it is gone.
    ayni_status_t destroy() {
        const ayni_status_t status = ayni_destroy(kernel_);
        if (status == AYNI_OK) {
            kernel_ = nullptr;
        }
        return status;
    }

    // Brings the kernel from any state to destroyed; harmless when there is
    // no kernel.
    ayni_status_t stop_and_destroy() {
        if (kernel_ == nullptr) {
            return AYNI_OK;
        }
        if (lifecycle() != AYNI_LIFECYCLE_CONFIGURING) {
            EXPECT_EQ(ayni_begin_shutdown(kernel_), AYNI_OK);
            // DRAINING appends the batches waiting one at a time.
            for (int round = 0; round < 10000 && lifecycle() != AYNI_LIFECYCLE_STOPPED; ++round) {
                sim_.complete_storage();
                step();
            }
        }
        return destroy();
    }

    // Destroys this kernel and creates another in the same memory over the
    // same log: a reboot.
    void reboot() {
        ASSERT_EQ(stop_and_destroy(), AYNI_OK);
        create_and_start();
    }

    // Power fails: each pending append leaves what append says, the batches
    // not yet appended are gone with the kernel's memory, and no request
    // completes. A new kernel then starts over the same log in new memory.
    void power_cut_and_start(ayni::hosts::sim::CutAppend append) {
        sim_.power_cut(append);
        kernel_ = nullptr;
        std::vector<uint64_t>().swap(memory_);
        create_and_start();
    }

  private:
    ayni::hosts::sim::Host sim_;
    ayni_config_v1 config_{};
    ayni_host_v1 table_{};
    std::function<void(ayni_kernel_t *)> registration_;
    std::vector<uint64_t> memory_;
    ayni_kernel_t *kernel_ = nullptr;
};

} // namespace ayni::test

#endif // AYNI_TESTS_UNIT_KERNEL_NODE_H
