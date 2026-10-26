#ifndef AYNI_RUNTIME_ASSISTANT_STATE_H
#define AYNI_RUNTIME_ASSISTANT_STATE_H
#include "conversation_store.h"
#include <array>
#include <ayni/calls.h>
#include <ayni/host_authority.h>
#include <ayni/runtime/native_node.h>
#include <ayni/runtime/native_node/assistant.h>
#include <cstring>
#include <optional>
namespace ayni::runtime {
// Contexts precede the node so normal destruction releases the Kernel first.
// An unsuccessful explicit close causes Assistant to retain the whole Impl.
struct Assistant::Impl {
    DemoProfile profile;
    NativeNode node;
    ConversationStore store;
    SavedConversation current;
    struct Saving {
        SavedConversation candidate;
        Notice notice;
    };
    std::optional<Saving> saving;
    ayni_status_t storage_status = AYNI_OK;
    uint64_t session = 0, boot = 0, command_sequence = 0;
    // Client receipt identities only; Core owns their effects and durable phase.
    std::array<uint64_t, AYNI_COMMAND_CAPACITY_DEFAULT> pending_commands{};
    uint32_t pending_count = 0;
    uint64_t rule_goal = 0;
    bool opened = false;
    bool shutting_down = false;
    ayni_status_t start(const std::string &, const ModelConfig &);
    ayni_status_t pump(std::vector<Notice> &);
    ayni_status_t collect(std::vector<Notice> &);
    ayni_status_t collect_receipts();
    ayni_status_t persist(SavedConversation, Notice notice = {});
    ayni_status_t stop();
    bool idle() const;
    Reply submit(ayni_intent_kind_t kind, const void *body, uint32_t revision = 0);
    Reply say(const Command &);
    Reply manage(const Command &);
    Reply describe(const Command &) const;
    Reply approve(uint64_t id, Operation operation);
    ayni_status_t ids(ayni_object_kind_t kind, std::vector<uint64_t> &out) const;
    uint64_t instance(uint64_t agent) const;
};
inline ayni_string_v1 runtime_text(const std::string &s) {
    return {s.data(), static_cast<uint32_t>(s.size())};
}
inline ayni_string_v1 runtime_text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
} // namespace ayni::runtime
#endif
