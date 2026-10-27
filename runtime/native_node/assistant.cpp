#include "assistant_state.h"
#include <chrono>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
namespace ayni::runtime {
namespace {
using SteadyClock = std::chrono::steady_clock;
constexpr auto boot_timeout = std::chrono::seconds(10);
constexpr uint32_t step_units = 64;
constexpr uint64_t step_us = 10000;
ayni_status_t unavailable_channel(void *, ayni_string_v1) {
    return AYNI_UNSUPPORTED;
}
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
ayni_status_t Assistant::open(const std::string &directory, const ModelConfig &config,
                              providers::presentation::Output output,
                              applications::assistant::QuietHours quiet) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    try {
        impl_ = std::make_unique<Impl>();
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = impl_->start(directory, config, output, std::move(quiet));
    if (status != AYNI_OK && impl_->stop() == AYNI_OK) {
        impl_.reset();
    }
    return status;
}
// Check the outer file under the Host lock before boot can write or dispatch anything.
// Legacy files lack source evidence and require an explicit future migration.
ayni_status_t Assistant::Impl::start(const std::string &directory, const ModelConfig &config,
                                     providers::presentation::Output output,
                                     applications::assistant::QuietHours quiet) {
    auto status = private_directory(directory);
    if (status != AYNI_OK) {
        return status;
    }
    status = node.assemble(directory.c_str(), demo_config());
    if (status != AYNI_OK) {
        return status;
    }
    const auto file_status = store.open(directory.c_str(), current);
    if (file_status != AYNI_OK && file_status != AYNI_NOT_FOUND) {
        return file_status;
    }
    if (!output.write) {
        output.write = unavailable_channel;
    }
    status = profile.assemble(node.kernel(), config,
                              {node.clock(), output, std::move(quiet), {this, propose}});
    if (status != AYNI_OK) {
        return status;
    }
    status = ayni_start(node.kernel());
    if (status != AYNI_OK) {
        return status;
    }
    const auto deadline = SteadyClock::now() + boot_timeout;
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
        if (SteadyClock::now() > deadline) {
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
        if (SteadyClock::now() > deadline) {
            return AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(2));
    } while (true);
    if (status != AYNI_OK) {
        return status;
    }
    status = initialize_context(file_status == AYNI_NOT_FOUND);
    if (status != AYNI_OK) {
        return status;
    }
    opened = true;
    return AYNI_OK;
}
uint64_t Assistant::Impl::assistant() const {
    ayni_agent_info_v1 info{};
    info.struct_size = sizeof(info);
    return ayni_get_agent(node.kernel(), demo_assistant, &info) == AYNI_OK ? info.instance_id : 0;
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
    if (shutting_down) {
        return AYNI_OK; // Preserve pending work; next open publishes only through the live anchor.
    }
    ayni_privacy_info_v1 privacy{};
    privacy.struct_size = sizeof(privacy);
    status = ayni_get_privacy_status(node.kernel(), session, &privacy);
    if (status != AYNI_OK || privacy.progress_status != AYNI_OK) {
        return status != AYNI_OK ? status : privacy.progress_status;
    }
    status = collect(notices);
    return status == AYNI_OK ? announce(notices) : status;
}
bool Assistant::Impl::idle() const {
    if (!opened || saving || awaiting_notice || pending_count || proposal_count) {
        return false;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    if (ayni_get_kernel_info(node.kernel(), &info) != AYNI_OK) {
        return false;
    }
    ayni_context_info_v1 context{};
    if (context_info(context) != AYNI_OK || context.pending_action != AYNI_CONTEXT_IDLE ||
        context.commit_seq > info.durable_seq || !context.usable || !matches(current, context)) {
        return false;
    }
    ayni_privacy_info_v1 privacy{};
    privacy.struct_size = sizeof(privacy);
    if (ayni_get_privacy_status(node.kernel(), session, &privacy) != AYNI_OK ||
        (privacy.phase != AYNI_PRIVACY_IDLE && privacy.phase != AYNI_PRIVACY_COMPLETE)) {
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
        bool deferred_notification = false;
        if (task.logical_call_id) {
            ayni_call_info_v1 call{};
            call.struct_size = sizeof(call);
            ayni_goal_info_v1 goal{};
            goal.struct_size = sizeof(goal);
            if (ayni_get_call(node.kernel(), task.logical_call_id, &call) != AYNI_OK ||
                ayni_get_goal(node.kernel(), task.goal_id, &goal) != AYNI_OK) {
                return false;
            }
            deferred_notification =
                call.wait_reason == AYNI_CALL_WAIT_OFFLINE &&
                std::string(goal.goal_type_id, goal.goal_type_id_size) == "assistant.reminder";
        }
        if (task.commit_seq > info.durable_seq || task.lifecycle == AYNI_TASK_READY ||
            (task.lifecycle == AYNI_TASK_RUNNING && !deferred_notification) ||
            task.lifecycle == AYNI_TASK_CANCELLING) {
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
        const auto clock = node.clock();
        const bool due_now = clock.now_us && goal.next_due_us != UINT64_MAX &&
                             goal.next_due_us <= clock.now_us(clock.context);
        // Future reminders/periods and subscription waits are retained work,
        // not a reason for /wait to block. A due first occurrence still is.
        if (goal.commit_seq > info.durable_seq ||
            (goal.lifecycle == AYNI_GOAL_ACTIVE && !goal.group_paused && !goal.last_task_id &&
             (due_now || goal.goal_id == current.pending_goal))) {
            return false;
        }
        if (goal.goal_id == current.pending_goal &&
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
    const auto deadline = SteadyClock::now() + boot_timeout;
    while (opened && state == AYNI_LIFECYCLE_RUNNING) {
        ayni_context_info_v1 context{};
        status = context_info(context);
        if (status != AYNI_OK) {
            return status;
        }
        if (!saving && !awaiting_notice && !pending_count &&
            context.pending_action == AYNI_CONTEXT_IDLE) {
            break;
        }
        status = pump(ignored);
        if (status != AYNI_OK || SteadyClock::now() > deadline) {
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
            if (SteadyClock::now() > deadline) {
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
        if (SteadyClock::now() > deadline) {
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
        if (SteadyClock::now() > deadline) {
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
