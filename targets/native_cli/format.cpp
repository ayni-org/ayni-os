#include "format.h"
#include <sstream>
namespace ayni::cli {
namespace {
const char *task_state(ayni_task_lifecycle_t state) {
    switch (state) {
    case AYNI_TASK_READY:
        return "就绪";
    case AYNI_TASK_RUNNING:
        return "执行中";
    case AYNI_TASK_PAUSED:
        return "已暂停";
    case AYNI_TASK_CANCELLING:
        return "取消中";
    case AYNI_TASK_COMPLETED:
        return "已完成";
    case AYNI_TASK_CANCELLED:
        return "已取消";
    case AYNI_TASK_FAILED:
        return "失败";
    case AYNI_TASK_EXPIRED:
        return "已到期";
    default:
        return "未知状态";
    }
}
const char *goal_state(ayni_goal_lifecycle_t state) {
    switch (state) {
    case AYNI_GOAL_PROPOSED:
        return "待审批";
    case AYNI_GOAL_ACTIVE:
        return "活动";
    case AYNI_GOAL_PAUSED:
        return "已暂停";
    case AYNI_GOAL_SATISFIED:
        return "已完成";
    case AYNI_GOAL_CANCELLED:
        return "已取消";
    case AYNI_GOAL_EXPIRED:
        return "已到期";
    case AYNI_GOAL_FAILED:
        return "失败";
    default:
        return "未知状态";
    }
}
} // namespace
std::string format(const runtime::Notice &notice) {
    switch (notice.kind) {
    case runtime::NoticeKind::answer:
        return "B：" + notice.body;
    case runtime::NoticeKind::rule_completed:
        return "A：规则检查完成（Mock，无硬件操作）。";
    case runtime::NoticeKind::turn_aborted:
        return "B：本轮未完成，保留原对话；请用 /tasks 查看原因。";
    case runtime::NoticeKind::context_erased:
        return "B：当前对话已清除；Kernel 历史正文擦除仍待 P3。";
    default:
        return {};
    }
}
std::string format(const runtime::Reply &reply, Action action, runtime::Operation operation) {
    std::ostringstream out;
    out << (reply.status == AYNI_OK ? "OK" : "ERROR") << " status=" << reply.status
        << " reason=" << reply.reason;
    if (reply.id) {
        out << " id=" << reply.id;
    }
    if (reply.commit_seq) {
        out << " commit=" << reply.commit_seq;
    }
    if (reply.command_id) {
        const auto *phase = reply.phase == AYNI_INTENT_DURABLE   ? "DURABLE"
                            : reply.phase == AYNI_INTENT_APPLIED ? "APPLIED"
                                                                 : "PENDING";
        out << " command=" << reply.command_id << " phase=" << phase;
    }
    if (reply.message == runtime::Message::accepted) {
        out << " 已受理；/wait 等待提交，/goals 查看是否待审批。";
    }
    if (reply.message == runtime::Message::rule_usage) {
        out << " A 使用预设规则：@a check；自由文本对话请用 @b。";
    }
    if (action == Action::wait) {
        out << (reply.status == AYNI_OK ? " 当前工作已收敛；待审批/暂停对象保留。"
                                        : " 等待已结束，工作仍保留。");
    }
    if (reply.status != AYNI_OK) {
        return out.str();
    }
    if (reply.kernel.struct_size) {
        out << " ABI " << AYNI_ABI_MAJOR << '.' << AYNI_ABI_MINOR
            << " boot=" << reply.kernel.boot_epoch << " durable_seq=" << reply.kernel.durable_seq
            << " lifecycle=" << reply.kernel.lifecycle
            << " restriction=" << reply.kernel.restriction
            << " 待提交命令=" << reply.pending_commands << " 对话轮次=" << reply.last_round
            << " 待处理Goal=" << reply.pending_goal << " 文件状态=" << reply.storage_status
            << (reply.saving ? "（写入中）" : "");
    }
    if (reply.conversation.generation) {
        out << " B 对话 generation=" << reply.conversation.generation
            << " revision=" << reply.conversation.revision;
        if (!reply.conversation.summary.empty()) {
            out << "\n未确认摘要：" << reply.conversation.summary;
        }
        for (const auto &turn : reply.conversation.turns) {
            out << "\n轮次 " << turn.number << "\n你：" << turn.user << "\nB：" << turn.answer;
        }
    }
    for (const auto &goal : reply.goals) {
        out << "\nGoal " << goal.goal_id << ' ' << goal_state(goal.lifecycle)
            << " owner=" << goal.owner_instance_id << " task=" << goal.last_task_id;
    }
    for (const auto &task : reply.tasks) {
        out << "\nTask " << task.task_id << ' ' << task_state(task.lifecycle)
            << " goal=" << task.goal_id << " model=" << task.model_calls_used << '/'
            << task.model_call_limit << " hold=" << task.hold_status << ':' << task.hold_reason;
    }
    for (const auto &call : reply.calls) {
        out << "\nCall " << call.logical_call_id << " task=" << call.task_id
            << " outcome=" << call.outcome << " status=" << call.status << " reason=" << call.reason
            << " query=" << call.query_status << ':' << call.query_reason << ':'
            << call.query_conclusion;
    }
    for (const auto &grant : reply.grants) {
        out << "\nGrant " << grant.grant_id << " lifecycle=" << grant.lifecycle
            << " subject=" << grant.subject_instance_id
            << " capability=" << std::string(grant.capability_id, grant.capability_id_size);
    }
    if ((operation == runtime::Operation::goals || operation == runtime::Operation::tasks ||
         operation == runtime::Operation::calls || operation == runtime::Operation::grants) &&
        reply.goals.empty() && reply.tasks.empty() && reply.calls.empty() && reply.grants.empty()) {
        out << " 无保留对象。";
    }
    return out.str();
}
} // namespace ayni::cli
