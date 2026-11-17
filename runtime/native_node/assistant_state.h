#ifndef AYNI_RUNTIME_ASSISTANT_STATE_H
#define AYNI_RUNTIME_ASSISTANT_STATE_H
#include "sqlite_values.h"
#include <array>
#include <ayni/calls.h>
#include <ayni/commands.hpp>
#include <ayni/context.h>
#include <ayni/host_authority.h>
#include <ayni/runtime/conversation.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/runtime/native_node.h>
#include <ayni/runtime/native_node/assistant.h>
#include <ayni/workspace.hpp>
#include <cstring>
#include <optional>
namespace ayni::runtime {
// Contexts precede the node so normal destruction releases the Kernel first.
// An unsuccessful explicit close causes Assistant to retain the whole Impl.
struct Assistant::Impl {
    ayni::Workspace conversation_workspace;
    SqliteValues store; // Adapter/worker outlive the Kernel and Session's borrowed ports.
    FileBodies bodies;
    conversation::Session conversation;
    DemoProfile profile;
    NativeNode node;
    uint64_t notified_generation = 0; // UI erasure notice cursor; zero before the initial load.
    ayni_status_t storage_status = AYNI_OK;
    uint64_t session = 0;
    CommandSequence command_ids; // Shared identity rule; CLI retains its multi-command window.
    // Client receipt identities only; Core owns their effects and durable phase.
    std::array<uint64_t, AYNI_COMMAND_CAPACITY_DEFAULT> pending_commands{};
    uint32_t pending_count = 0;
    // Model Goals waiting for approval that the owner has not been told about since open: every
    // one found at open, then each the schedule tool registers. A Goal is announced once its
    // creation is durable (ADR-0072). The tool keeps at most model_goals_waiting_max waiting.
    std::array<uint64_t, applications::assistant::model_goals_waiting_max> proposals{};
    uint32_t proposal_count = 0;
    bool opened = false;
    bool shutting_down = false;
    ayni_status_t start(const std::string &, const ModelConfig &, providers::presentation::Output,
                        applications::assistant::QuietHours);
    ayni_status_t advance_kernel(); // Progress only; never consumes UI observations.
    ayni_status_t pump(std::vector<Notice> &);
    ayni_status_t collect(std::vector<Notice> &);
    ayni_status_t context_info(ayni_context_info_v1 &) const;
    ayni_status_t initialize_context();
    ayni_status_t collect_receipts();
    ayni_status_t announce(std::vector<Notice> &);
    static ayni_status_t propose(void *, const ayni_goal_create_v1 *, uint64_t *);
    ayni_status_t stop();
    bool idle() const;
    Reply submit(ayni_intent_kind_t kind, const void *body, uint32_t revision = 0);
    Reply say(const Command &);
    Reply cancel_pending_turn();
    Reply stop_work();
    Reply set_autonomy(const ayni_pause_group_info_v1 &, bool paused);
    Reply application_goal(const Command &);
    Reply manage(const Command &);
    Reply memory(const Command &);
    Reply describe(const Command &) const;
    Reply approve(uint64_t id, Operation operation);
    // Immutable creation echo of a retained Goal, read back through public queries only. body
    // views parameters and calendar, so a Creation is filled in place and never copied.
    struct Creation {
        Creation() = default;
        Creation(const Creation &) = delete;
        Creation &operator=(const Creation &) = delete;
        strategies::Encoded parameters;
        ayni_calendar_info_v1 calendar{};
        ayni_goal_info_v1 info{};
        ayni_goal_create_v1 body{};
    };
    ayni_status_t creation(uint64_t id, Creation &) const;
    ayni_status_t ids(ayni_object_kind_t kind, std::vector<uint64_t> &out) const;
    uint64_t assistant() const; // Current instance of the one assistant Agent.
};
inline ayni_string_v1 runtime_text(const std::string &s) {
    return {s.data(), static_cast<uint32_t>(s.size())};
}
inline ayni_string_v1 runtime_text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
} // namespace ayni::runtime
#endif
