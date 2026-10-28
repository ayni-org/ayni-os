#include "assistant_state.h"
namespace ayni::runtime {
// Owner Goals: the shared schedule grammar (ADR-0072), or organize over explicit sources.
Reply Assistant::Impl::application_goal(const Command &command) {
    using namespace applications::assistant;
    Reply reply;
    Schedule schedule;
    if (command.operation == Operation::schedule) {
        reply.status = parse_schedule(command.text, schedule);
    } else {
        schedule.parameters.kind = GoalKind::memory_organize;
        schedule.parameters.sources = command.sources;
    }
    Encoded encoded;
    ayni_goal_create_v1 body{};
    if (reply.status == AYNI_OK) {
        reply.status = prepare_goal(schedule, assistant(), encoded, body);
    }
    if (reply.status != AYNI_OK) {
        return reply;
    }
    // Each explicit owner command is a new intention. Parameter equality alone
    // must not merge two reminders the user deliberately creates (C-WIRE-005).
    return submit(AYNI_INTENT_GOAL_CREATE, &body);
}
// A schedule tool Goal is announced once its latest change is durable, with the canonical text
// read back from Kernel rather than anything the model said. A Goal already gone, or already
// approved or cancelled, needs no approval prompt and is dropped silently.
ayni_status_t Assistant::Impl::announce(std::vector<Notice> &notices) {
    ayni_kernel_info_v1 kernel{};
    kernel.struct_size = sizeof(kernel);
    auto status = ayni_get_kernel_info(node.kernel(), &kernel);
    for (uint32_t at = 0; status == AYNI_OK && at < proposal_count;) {
        Creation goal;
        status = creation(proposals[at], goal);
        // Tracked parameters answer BUSY until creation is durable; both mean "not yet".
        if (status == AYNI_BUSY ||
            (status == AYNI_OK && goal.info.commit_seq > kernel.durable_seq)) {
            status = AYNI_OK;
            ++at;
            continue;
        }
        if (status == AYNI_OK && goal.info.lifecycle == AYNI_GOAL_PROPOSED) {
            Notice notice{NoticeKind::goal_proposed, {}, proposals[at]};
            status = applications::assistant::format_schedule(goal.body, notice.body);
            if (status == AYNI_OK) {
                notices.push_back(std::move(notice));
            }
        }
        if (status == AYNI_OK || status == AYNI_NOT_FOUND) {
            proposals[at] = proposals[--proposal_count];
            status = AYNI_OK;
        }
    }
    return status;
}
} // namespace ayni::runtime
