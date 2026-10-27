#include "assistant_state.h"
namespace ayni::runtime {
Reply Assistant::Impl::application_goal(const Command &command) {
    using namespace applications::assistant;
    Reply reply;
    if (command.agent != demo_agent_a && command.agent != demo_agent_b) {
        reply.status = AYNI_INVALID_ARGUMENT;
        return reply;
    }
    Parameters parameters;
    parameters.kind = command.operation == Operation::remind            ? GoalKind::reminder
                      : command.operation == Operation::device_check    ? GoalKind::device_check
                      : command.operation == Operation::memory_organize ? GoalKind::memory_organize
                                                                        : GoalKind::scene_observe;
    parameters.text = command.text;
    parameters.sources = command.sources;
    Encoded encoded;
    ayni_goal_create_v1 body{};
    reply.status = prepare_goal(
        parameters, instance(command.agent), command.period_us,
        command.operation == Operation::remind ? &command.calendar : nullptr, encoded, body);
    if (reply.status != AYNI_OK) {
        return reply;
    }
    // Each explicit owner command is a new intention. Parameter equality alone
    // must not merge two reminders the user deliberately creates (C-WIRE-005).
    return submit(AYNI_INTENT_GOAL_CREATE, &body);
}
} // namespace ayni::runtime
