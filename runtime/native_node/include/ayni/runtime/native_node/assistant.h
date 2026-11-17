// P2 Native application runtime. Its caller is the single Kernel owner thread;
// CLI parsing, input/output threads and presentation are outside this component.
#ifndef AYNI_RUNTIME_ASSISTANT_H
#define AYNI_RUNTIME_ASSISTANT_H
#include <ayni/calls.h>
#include <ayni/context.h>
#include <ayni/memory.h>
#include <ayni/runtime/common/demo_profile.h>
#include <memory>
#include <string>
#include <vector>
namespace ayni::runtime {
// say starts a model turn unless its text is a stop phrase; stop and stop phrases take the
// deterministic stop path instead (ADR-0072). schedule registers an owner Goal from the
// shared grammar; memory_organize names its explicit memory sources.
enum class Operation {
    say,
    stop,
    schedule,
    memory_organize,
    pause_assistant,
    resume_assistant,
    notifications,
    diagnostics,
    status,
    goals,
    tasks,
    calls,
    grants,
    grant,
    revoke,
    approve,
    reauthorize,
    raise_limit,
    resume_wait,
    query_result,
    cancel,
    conversation,
    erase,
    memory_list,
    memory_show,
    memory_save,
    memory_edit,
    memory_confirm,
    memory_delete,
    memory_share,
    memory_export,
    profile_list,
    profile_save
};
// One owner request to the single assistant. text is the say input, the schedule grammar
// line (applications::assistant::parse_schedule), a grant alias or a memory body; sources
// are the explicit memories an organize Goal reads.
struct Command {
    Operation operation = Operation::status;
    uint64_t id = 0;
    uint32_t limit = 0;
    ayni_profile_field_t profile_field = AYNI_PROFILE_NONE;
    std::string text;
    std::vector<uint64_t> sources;
    ayni_memory_purpose_t purpose = AYNI_MEMORY_CONTEXT;
};
// goal_proposed: the model registered Goal id, durable and waiting for owner approval; body is
// its canonical schedule text, built by the runtime from the request, never by the model.
enum class NoticeKind { none, answer, turn_aborted, context_erased, goal_proposed };
struct Notice {
    NoticeKind kind = NoticeKind::none;
    std::string body;
    uint64_t id = 0; // Goal ID for goal_proposed, zero otherwise.
};
struct ConversationTurn {
    uint64_t number;
    std::string user, answer;
};
struct ConversationView {
    uint64_t generation = 0, revision = 0;
    std::string summary, draft;
    std::vector<ConversationTurn> turns;
};
// stopped: the deterministic stop path ran; the turn is cancelled and autonomy paused.
enum class Message { none, accepted, stopped };
struct MemoryEntry {
    ayni_memory_info_v1 info{};
    std::string body;
};
struct Reply {
    ayni_status_t status = AYNI_OK;
    ayni_reason_t reason = AYNI_REASON_NONE;
    uint64_t id = 0;
    uint64_t commit_seq = 0;       // Persistence barrier; zero for queries or PENDING intents.
    uint64_t command_id = 0;       // Accepted management key; zero for queries.
    ayni_intent_phase_t phase = 0; // Public receipt phase; zero for queries.
    Message message = Message::none;
    ayni_kernel_info_v1 kernel{};
    ayni_privacy_info_v1 privacy{};
    ayni_context_info_v1 context{};
    uint64_t last_round = 0, pending_goal = 0;
    ayni_status_t storage_status = AYNI_OK;
    bool saving = false;        // Original input staging or file read/write is still pending.
    bool context_ready = false; // Readable current record or attached original Core turn.
    uint32_t pending_commands = 0;
    applications::assistant::Activity activity;
    std::vector<applications::assistant::Presentation> presentations;
    ConversationView conversation;
    std::vector<ayni_goal_info_v1> goals;
    std::vector<ayni_task_info_v1> tasks;
    std::vector<ayni_call_info_v1> calls;
    std::vector<ayni_grant_info_v1> grants;
    std::vector<MemoryEntry> memories;
};
class Assistant {
  public:
    Assistant();
    ~Assistant();
    Assistant(const Assistant &) = delete;
    Assistant &operator=(const Assistant &) = delete;
    // Directory must be owner-private; creates missing directories with mode 0700.
    // Boot/registration finishes before OK; advance completes local loading (context_ready).
    // No auto-grants. A restored original turn continues asynchronously after loading.
    // Without a channel callback notifications fail explicitly with UNSUPPORTED.
    ayni_status_t open(const std::string &directory, const ModelConfig &config,
                       providers::presentation::Output output = {},
                       applications::assistant::QuietHours quiet = {});
    Reply execute(const Command &command);
    // Bounded Kernel step plus polling only; file/network I/O use workers. Replies
    // are published only after their conversation candidate has been saved.
    ayni_status_t advance(std::vector<Notice> &notices);
    bool idle() const; // No runnable turn, pending save or uncommitted accepted intent.
    void wait();       // Owner may wait briefly for Host wake; does not touch stdin.
    // Drains asynchronous borrows and saves completed context before destruction.
    // Failure retains the instance for diagnosis; never frees a live Kernel.
    ayni_status_t close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::runtime
#endif
