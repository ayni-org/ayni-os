// Native SQLite conversation/history owner. The Host directory lock spans open
// through close; startup checks run before use, then only the worker accesses SQL.
#ifndef AYNI_RUNTIME_CONVERSATION_STORE_H
#define AYNI_RUNTIME_CONVERSATION_STORE_H
#include <array>
#include <ayni/strategies/builtin.h>
#include <condition_variable>
#include <mutex>
#include <thread>
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
    void close(); // Joins worker; call only after consuming the last save.
  private:
    enum class Phase { idle, queued, writing, done, stopping };
    static constexpr uint32_t file_size_max = strategies::state_bytes_max;
    using Buffer = std::array<uint8_t, file_size_max>;
    ayni_status_t read(SavedConversation &state);
    ayni_status_t write();
    struct HistoryJob {
        Phase phase = Phase::idle;
        uint64_t task = 0;
        uint64_t turn = 0;
        strategies::ConversationRecord record;
        uint64_t reference = 0;
        ayni_status_t status = AYNI_OK;
    };
    static uint32_t history_ready(void *, uint64_t);
    static ayni_status_t history_load(void *, uint64_t, const strategies::ConversationRecord &,
                                      strategies::Text<strategies::history_bytes_max> &);
    static ayni_status_t history_save(void *, uint64_t, uint64_t,
                                      const strategies::ConversationRecord &, uint64_t &);
    ayni_status_t read_history(HistoryJob &);
    ayni_status_t write_history(HistoryJob &);
    ayni_status_t current_namespace(const strategies::ConversationRecord &);
    void run();
    int directory_ = -1;
    sqlite3 *database_ = nullptr;
    SavedConversation pending_;
    Erasure erasure_ = Erasure::none;
    uint64_t erased_turn_ = 0;
    uint64_t conversation_ = 0, owner_ = 0, generation_ = 0; // Last committed SQLite namespace.
    HistoryJob loading_, storing_;
    struct ResultJob {
        static constexpr uint32_t raw_bytes_max = 2 * AYNI_CALL_RESULT_BYTES_MAX +
                                                  AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX +
                                                  AYNI_BUSINESS_CONTRACT_BYTES_MAX;
        static constexpr uint32_t json_bytes_max =
            1048576; // Includes worst-case string/key escaping.
        Phase phase = Phase::idle;
        uint64_t task = 0;
        uint64_t conversation = 0, owner = 0, generation = 0;
        ayni_task_call_result_v1 result{};
        std::array<uint8_t, raw_bytes_max> raw{};
        std::array<char, json_bytes_max> json{};
        strategies::ArchivedResult archived;
        ayni_status_t status = AYNI_OK;
    } archiving_;
    static ayni_status_t archive_result(void *, uint64_t, const strategies::ConversationRecord &,
                                        const ayni_task_call_result_v1 &,
                                        strategies::ArchivedResult &);
    static ayni_status_t result_chunk(void *, uint64_t, uint64_t, uint32_t,
                                      strategies::Text<strategies::prompt_bytes_max> &, uint32_t);
    ayni_status_t write_result();
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
