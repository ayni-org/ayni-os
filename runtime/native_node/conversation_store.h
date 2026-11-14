// Native SQLite conversation, body store (ADR-0096) and result archive owner. The Host directory
// lock spans open through close; startup checks run before use, then only the worker accesses
// SQL. Every live body is also cached in memory, so reads never wait for SQL.
#ifndef AYNI_RUNTIME_CONVERSATION_STORE_H
#define AYNI_RUNTIME_CONVERSATION_STORE_H
#include <array>
#include <ayni/strategies/builtin.h>
#include <ayni/workspace.hpp>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct sqlite3;
namespace ayni::runtime {
struct SavedConversation {
    strategies::ConversationRecord record;
    uint64_t context_id =
        0; // Kernel anchor identity; record generation/revision mirror privacy/file versions.
    uint64_t pending_goal = 0; // Zero when no turn is outstanding; otherwise exact Goal ID.
};
class ConversationStore {
  public:
    enum class Erasure { none, all, derived, turn };
    ConversationStore() = default;
    ~ConversationStore();
    // Startup-only read. NOT_FOUND preserves state; a new store is explicit.
    ayni_status_t open(const char *directory, SavedConversation &state);
    // One bounded request. BUSY retains nothing; accepted data is copied.
    ayni_status_t save(const SavedConversation &state, Erasure erasure = Erasure::none,
                       uint64_t turn = 0);
    // True consumes a completed write; false leaves status unchanged.
    bool poll(ayni_status_t &status);
    strategies::HistoryPort history_port();
    // Asynchronous writes for decide and Provider threads: NOT_READY until SQL committed.
    const ayni_body_store_v1 *bodies() const { return &bodies_port_; }
    // Same store whose write waits for the commit, for owner-thread calls outside decide.
    const ayni_body_store_v1 *waiting_bodies() const { return &waiting_port_; }
    void close(); // Joins worker; call only after consuming the last save.
  private:
    enum class Phase { idle, queued, writing, done, stopping };
    static constexpr uint32_t file_size_max = strategies::encoded_bytes_max;
    using Buffer = std::array<uint8_t, file_size_max>;
    ayni_status_t read(SavedConversation &state);
    ayni_status_t write();
    struct Body {
        uint64_t scope = 0;
        uint64_t digest = 0;
        std::shared_ptr<const std::string> bytes;
        uint64_t content = 0; // Shared immutable payload row, independent of each scope reference.
        enum class State { staged, pending, durable, failed } state = State::pending;
    };
    static uint32_t history_ready(void *, uint64_t);
    static ayni_status_t body_stage(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t body_share(void *, uint64_t, const ayni_body_ref_v1 *, ayni_body_ref_v1 *);
    static ayni_status_t body_confirm(void *, const ayni_body_ref_v1 *);
    static ayni_status_t body_write(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t body_write_waiting(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t body_read(void *, const ayni_body_ref_v1 *, uint8_t *, uint32_t);
    static void body_release_temporary(void *, const ayni_body_ref_v1 *);
    static void body_release(void *, const ayni_body_ref_v1 *);
    ayni_status_t load_bodies();
    ayni_status_t write_bodies(std::vector<std::pair<uint64_t, Body>> &, std::vector<uint64_t> &);
    ayni_status_t current_namespace(const strategies::ConversationRecord &);
    void run();
    int directory_ = -1;
    sqlite3 *database_ = nullptr;
    SavedConversation pending_;
    Erasure erasure_ = Erasure::none;
    uint64_t erased_turn_ = 0;
    uint64_t conversation_ = 0, owner_ = 0, generation_ = 0; // Last committed SQLite namespace.
    std::map<uint64_t, Body> bodies_; // Every live body; pending ones await the worker.
    std::vector<uint64_t> inserts_;   // Pending bodies in write order.
    std::vector<uint64_t> deletes_;   // Released bodies to remove from SQL.
    static constexpr size_t body_reference_limit = 4096;
    static constexpr size_t body_cache_limit = 16U * 1024U * 1024U;
    ayni_status_t body_status_ = AYNI_OK; // Sticky worker body I/O failure, including removals.
    uint64_t next_body_ = 1;              // Next identity; never reused within the database.
    uint64_t collect_below_ = 0;          // Bodies older than the queued save it may collect.
    std::condition_variable durable_;     // Signalled after each body batch.
    ayni_body_store_v1 bodies_port_{sizeof(ayni_body_store_v1),
                                    0,
                                    this,
                                    body_write,
                                    body_read,
                                    body_release,
                                    body_stage,
                                    body_share,
                                    body_confirm,
                                    body_release_temporary};
    ayni_body_store_v1 waiting_port_{sizeof(ayni_body_store_v1),
                                     0,
                                     this,
                                     body_write_waiting,
                                     body_read,
                                     body_release,
                                     body_stage,
                                     body_share,
                                     body_confirm,
                                     body_release_temporary};
    struct ResultJob {
        Phase phase = Phase::idle;
        uint64_t task = 0;
        uint64_t conversation = 0, owner = 0, generation = 0;
        ayni_task_call_result_v1 result{};
        std::vector<uint8_t> raw;
        std::string json;
        strategies::ArchivedResult archived;
        ayni_status_t status = AYNI_OK;
    } archiving_;
    static ayni_status_t archive_result(void *, uint64_t, const strategies::ConversationRecord &,
                                        const ayni_task_call_result_v1 &,
                                        strategies::ArchivedResult &);
    static ayni_status_t result_chunk(void *, uint64_t, uint64_t, uint32_t, std::string &,
                                      uint32_t);
    ayni_status_t write_result();
    ayni_status_t render(uint64_t reference, size_t &size);
    ayni::Workspace owner_workspace_;  // Save encoder: owner thread only.
    ayni::Workspace worker_workspace_; // Startup read, then SQLite worker only.
    ayni::Workspace result_workspace_; // Result rendering: SQLite worker only.
    Buffer bytes_{};
    uint32_t size_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    Phase phase_ = Phase::idle;
    ayni_status_t status_ = AYNI_OK;
    std::thread worker_;
};
} // namespace ayni::runtime
#endif
