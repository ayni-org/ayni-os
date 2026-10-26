#include "assistant_state.h"
#include <chrono>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
namespace ayni::runtime {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto boot_timeout = std::chrono::seconds(10);
constexpr uint32_t step_units = 64;
constexpr uint64_t step_us = 10000;
constexpr uint64_t conversation_id = 1;
ayni_status_t private_directory(const std::string &path) {
    std::error_code error;
    const bool made = std::filesystem::create_directories(path, error);
    if (error || (made && chmod(path.c_str(), 0700) != 0)) {
        return AYNI_STORAGE_FAILED;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) != 0) {
        return AYNI_DENIED;
    }
    return AYNI_OK;
}
} // namespace
Assistant::Assistant() = default;
Assistant::~Assistant() {
    if (impl_ && impl_->stop() != AYNI_OK) {
        // A failed drain does not end Host/Provider borrows (C-HOST-018).
        static_cast<void>(impl_.release());
    }
}
ayni_status_t Assistant::open(const std::string &directory, const ModelConfig &config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    try {
        impl_ = std::make_unique<Impl>();
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = impl_->start(directory, config);
    if (status != AYNI_OK && impl_->stop() == AYNI_OK) {
        impl_.reset();
    }
    return status;
}
// Startup loads the outer record only under the Host's directory lock, after
// Kernel identity recovery. A missing/mismatched record never adopts old work.
ayni_status_t Assistant::Impl::start(const std::string &directory, const ModelConfig &config) {
    auto status = private_directory(directory);
    if (status != AYNI_OK) {
        return status;
    }
    status = node.assemble(directory.c_str(), demo_config());
    if (status != AYNI_OK) {
        return status;
    }
    status = profile.assemble(node.kernel(), config);
    if (status != AYNI_OK) {
        return status;
    }
    status = ayni_start(node.kernel());
    if (status != AYNI_OK) {
        return status;
    }
    const auto deadline = Clock::now() + boot_timeout;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_RECOVERING;
    while (lifecycle == AYNI_LIFECYCLE_RECOVERING) {
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        status = ayni_step(node.kernel(), step_units, step_us, &report);
        if (status != AYNI_OK) {
            return status;
        }
        status = ayni_get_lifecycle(node.kernel(), &lifecycle);
        if (status != AYNI_OK) {
            return status;
        }
        if (Clock::now() > deadline) {
            return AYNI_BUSY;
        }
        if (!report.more_ready) {
            node.wait_for_wake(std::chrono::milliseconds(2));
        }
    }
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    status = ayni_get_kernel_info(node.kernel(), &info);
    if (status != AYNI_OK) {
        return status;
    }
    boot = info.boot_epoch;
    const ayni_session_desc_v1 descriptor{sizeof(descriptor), AYNI_CHANNEL_MANAGEMENT, demo_owner,
                                          demo_binding_revision, 0};
    do {
        status = ayni_open_session(node.kernel(), &descriptor, &session);
        if (status != AYNI_NOT_READY) {
            break;
        }
        // RUNNING precedes the initial owner-binding durability barrier.
        // C-AUTH-010 admits a management session only after that barrier.
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        status = ayni_step(node.kernel(), step_units, step_us, &report);
        if (status != AYNI_OK) {
            return status;
        }
        if (Clock::now() > deadline) {
            return AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    } while (true);
    if (status != AYNI_OK) {
        return status;
    }
    status = store.open(directory.c_str(), current);
    if (status == AYNI_NOT_FOUND) {
        std::vector<uint64_t> goals;
        status = ids(AYNI_OBJECT_GOAL, goals);
        if (status != AYNI_OK) {
            return status;
        }
        for (const auto id : goals) {
            ayni_goal_info_v1 goal{};
            goal.struct_size = sizeof(goal);
            status = ayni_get_goal(node.kernel(), id, &goal);
            if (status != AYNI_OK) {
                return status;
            }
            if (goal.owner_instance_id == instance(demo_agent_b)) {
                return AYNI_NOT_READY;
            }
        }
        current.record.id = conversation_id;
        current.record.owner_instance_id = instance(demo_agent_b);
        status = persist(current);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (current.record.id != conversation_id ||
        current.record.owner_instance_id != instance(demo_agent_b)) {
        return AYNI_DENIED;
    }
    std::vector<uint64_t> retained;
    status = ids(AYNI_OBJECT_GOAL, retained);
    if (status != AYNI_OK) {
        return status;
    }
    for (const auto id : retained) {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        status = ayni_get_goal(node.kernel(), id, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.owner_instance_id == instance(demo_agent_a) &&
            (goal.lifecycle == AYNI_GOAL_PROPOSED || goal.lifecycle == AYNI_GOAL_ACTIVE ||
             goal.lifecycle == AYNI_GOAL_PAUSED)) {
            if (rule_goal) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            rule_goal = id;
        }
    }
    opened = true;
    return AYNI_OK;
}
uint64_t Assistant::Impl::instance(uint64_t agent) const {
    ayni_agent_info_v1 info{};
    info.struct_size = sizeof(info);
    return ayni_get_agent(node.kernel(), agent, &info) == AYNI_OK ? info.instance_id : 0;
}
ayni_status_t Assistant::Impl::ids(ayni_object_kind_t kind, std::vector<uint64_t> &out) const {
    uint32_t count = 0;
    auto status = ayni_list_objects(node.kernel(), kind, nullptr, 0, &count);
    if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    out.resize(count);
    return ayni_list_objects(node.kernel(), kind, out.data(), count, &count);
}
ayni_status_t Assistant::Impl::persist(SavedConversation candidate, Notice notice) {
    if (saving) {
        return AYNI_BUSY;
    }
    const auto status = store.save(candidate);
    if (status == AYNI_OK) {
        saving = Saving{std::move(candidate), std::move(notice)};
    } else {
        storage_status = status;
    }
    return status;
}
// Apply a saved candidate before inspecting new completions. Admission stays
// closed while saving, so no CLI-created Goal can reclaim an unexported Task.
ayni_status_t Assistant::Impl::collect(std::vector<Notice> &notices) {
    if (saving) {
        ayni_status_t result = AYNI_OK;
        if (!store.poll(result)) {
            return AYNI_OK;
        }
        if (result != AYNI_OK) {
            storage_status = result;
            saving.reset();
            return result;
        }
        current = saving->candidate;
        if (saving->notice.kind != NoticeKind::none) {
            notices.push_back(saving->notice);
        }
        saving.reset();
    }
    if (storage_status != AYNI_OK) {
        return storage_status;
    }
    if (!current.pending_goal) {
        return AYNI_OK;
    }
    ayni_goal_info_v1 goal{};
    goal.struct_size = sizeof(goal);
    auto status = ayni_get_goal(node.kernel(), current.pending_goal, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    if (goal.owner_instance_id != current.record.owner_instance_id) {
        return AYNI_DENIED;
    }
    if (!goal.last_task_id) {
        if (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_EXPIRED ||
            goal.lifecycle == AYNI_GOAL_FAILED) {
            auto candidate = current;
            candidate.pending_goal = 0;
            return persist(candidate, {NoticeKind::turn_aborted, {}});
        }
        return AYNI_OK;
    }
    ayni_task_info_v1 task{};
    task.struct_size = sizeof(task);
    status = ayni_get_task(node.kernel(), goal.last_task_id, &task);
    if (status != AYNI_OK) {
        return status;
    }
    if (task.lifecycle == AYNI_TASK_COMPLETED) {
        strategies::Encoded encoded;
        status = ayni_copy_task_checkpoint(node.kernel(), task.task_id, encoded.bytes.data(),
                                           strategies::state_bytes_max, &encoded.size);
        if (status == AYNI_BUSY) {
            return AYNI_OK;
        }
        if (status != AYNI_OK) {
            return status;
        }
        auto candidate = current;
        status = strategies::Conversation::finish(current.record, encoded.view(), candidate.record);
        if (status != AYNI_OK) {
            return status;
        }
        candidate.pending_goal = 0;
        const auto &answer = candidate.record.rounds[candidate.record.round_count - 1].answer;
        return persist(candidate,
                       {NoticeKind::answer, std::string(answer.bytes.data(), answer.size)});
    }
    if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_CANCELLED ||
        task.lifecycle == AYNI_TASK_EXPIRED) {
        auto candidate = current;
        candidate.pending_goal = 0;
        return persist(candidate, {NoticeKind::turn_aborted, {}});
    }
    return AYNI_OK;
}
ayni_status_t Assistant::Impl::pump(std::vector<Notice> &notices) {
    auto status = profile.advance();
    if (status != AYNI_OK) {
        return status;
    }
    ayni_step_report_v1 report{};
    report.struct_size = sizeof(report);
    status = ayni_step(node.kernel(), step_units, step_us, &report);
    if (status != AYNI_OK) {
        return status;
    }
    ayni_lifecycle_t lifecycle = 0;
    status = ayni_get_lifecycle(node.kernel(), &lifecycle);
    if (status != AYNI_OK) {
        return status;
    }
    if (lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return AYNI_OK;
    }
    status = collect_receipts();
    if (status != AYNI_OK) {
        return status;
    }
    status = collect(notices);
    if (status != AYNI_OK) {
        return status;
    }
    if (rule_goal) {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        status = ayni_get_goal(node.kernel(), rule_goal, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        ayni_kernel_info_v1 info{};
        info.struct_size = sizeof(info);
        status = ayni_get_kernel_info(node.kernel(), &info);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.lifecycle == AYNI_GOAL_SATISFIED && goal.commit_seq <= info.durable_seq) {
            notices.push_back({NoticeKind::rule_completed, {}});
            rule_goal = 0;
        } else if (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED ||
                   goal.lifecycle == AYNI_GOAL_EXPIRED) {
            rule_goal = 0;
        }
    }
    return AYNI_OK;
}
bool Assistant::Impl::idle() const {
    if (!opened || saving || pending_count) {
        return false;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    if (ayni_get_kernel_info(node.kernel(), &info) != AYNI_OK) {
        return false;
    }
    std::vector<uint64_t> objects;
    if (ids(AYNI_OBJECT_TASK, objects) != AYNI_OK) {
        return false;
    }
    for (const auto id : objects) {
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        if (ayni_get_task(node.kernel(), id, &task) != AYNI_OK) {
            return false;
        }
        if (task.commit_seq > info.durable_seq || task.lifecycle == AYNI_TASK_READY ||
            task.lifecycle == AYNI_TASK_RUNNING || task.lifecycle == AYNI_TASK_CANCELLING) {
            return false;
        }
        // Completion is not yet a saved conversation: collect must cross both
        // the Kernel durable barrier and the outer file commit before /wait ends.
        if (task.goal_id == current.pending_goal && task.lifecycle != AYNI_TASK_PAUSED) {
            return false;
        }
    }
    if (ids(AYNI_OBJECT_GOAL, objects) != AYNI_OK) {
        return false;
    }
    for (const auto id : objects) {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        if (ayni_get_goal(node.kernel(), id, &goal) != AYNI_OK) {
            return false;
        }
        if (goal.commit_seq > info.durable_seq ||
            (goal.lifecycle == AYNI_GOAL_ACTIVE && !goal.last_task_id)) {
            return false;
        }
        if ((goal.goal_id == current.pending_goal || goal.goal_id == rule_goal) &&
            (goal.lifecycle == AYNI_GOAL_SATISFIED || goal.lifecycle == AYNI_GOAL_CANCELLED ||
             goal.lifecycle == AYNI_GOAL_EXPIRED || goal.lifecycle == AYNI_GOAL_FAILED)) {
            return false;
        }
    }
    return true;
}
ayni_status_t Assistant::Impl::stop() {
    if (!node.kernel()) {
        store.close();
        return profile.close();
    }
    ayni_lifecycle_t state = 0;
    auto status = ayni_get_lifecycle(node.kernel(), &state);
    if (status != AYNI_OK) {
        return status;
    }
    std::vector<Notice> ignored;
    const auto deadline = Clock::now() + boot_timeout;
    while (saving) {
        status = collect(ignored);
        if (status != AYNI_OK || Clock::now() > deadline) {
            return status != AYNI_OK ? status : AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    }
    if (state != AYNI_LIFECYCLE_CONFIGURING && state != AYNI_LIFECYCLE_STOPPED) {
        status = ayni_begin_shutdown(node.kernel());
        if (status != AYNI_OK) {
            return status;
        }
        shutting_down = true;
        while (state != AYNI_LIFECYCLE_STOPPED) {
            status = pump(ignored);
            if (status != AYNI_OK) {
                return status;
            }
            status = ayni_get_lifecycle(node.kernel(), &state);
            if (status != AYNI_OK) {
                return status;
            }
            if (Clock::now() > deadline) {
                return AYNI_BUSY;
            }
            node.wait_for_wake(std::chrono::milliseconds(2));
        }
    }
    // A save queued by the last running step must finish before releasing the lock.
    while (saving) {
        ayni_status_t result;
        if (store.poll(result)) {
            if (result != AYNI_OK) {
                return result;
            }
            current = saving->candidate;
            saving.reset();
        }
        if (Clock::now() > deadline) {
            return AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    }
    store.close();
    // STOPPED accounts for consumed completions, but producer post() may still
    // be returning through Host wake. Join producers before releasing either
    // Kernel memory or Host, while their registered contexts remain allocated.
    do {
        status = profile.quiesce();
        if (status != AYNI_BUSY) {
            break;
        }
        if (Clock::now() > deadline) {
            return AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    } while (true);
    if (status != AYNI_OK) {
        return status;
    }
    status = node.release();
    return status == AYNI_OK ? profile.close() : status;
}
ayni_status_t Assistant::advance(std::vector<Notice> &notices) {
    return impl_ && impl_->opened ? impl_->pump(notices) : AYNI_INVALID_STATE;
}
bool Assistant::idle() const {
    return impl_ && impl_->idle();
}
void Assistant::wait() {
    if (impl_) {
        impl_->node.wait_for_wake(std::chrono::milliseconds(2));
    }
}
ayni_status_t Assistant::close() {
    if (!impl_) {
        return AYNI_OK;
    }
    const auto status = impl_->stop();
    if (status == AYNI_OK) {
        impl_.reset();
    }
    return status;
}
} // namespace ayni::runtime
