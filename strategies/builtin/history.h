// Conversation-owned message index; payloads are immutable shared bodies (ADR-0104).
#ifndef AYNI_STRATEGIES_HISTORY_H
#define AYNI_STRATEGIES_HISTORY_H
#include "support.h"
namespace ayni::strategies::detail {
// Saves only the newly appended suffix. Replacement/compaction resets the prefix cursor.
ayni_status_t store_history(const ayni_body_store_v1 &, Turn &);
ayni_status_t load_history(const ayni_body_store_v1 &, const BodyRef &, std::string &,
                           std::vector<BodyRef> *references = nullptr);
// Shared operation reader avoids re-copying or re-verifying discovered dependencies.
ayni_status_t load_history(BodyReader &, const BodyRef &, std::string &,
                           std::vector<BodyRef> *references = nullptr);
ayni_status_t record_references(BodyReader &, const ConversationRecord &, std::vector<BodyRef> &);
ayni_status_t record_references(const ayni_body_store_v1 &, const ConversationRecord &,
                                std::vector<BodyRef> &);
} // namespace ayni::strategies::detail
#endif
