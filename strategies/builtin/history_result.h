// Derived JSON view references the original normalized Core result and its saved Schema.
#ifndef AYNI_STRATEGIES_HISTORY_RESULT_H
#define AYNI_STRATEGIES_HISTORY_RESULT_H
#include "support.h"
namespace ayni::strategies::detail {
ayni_status_t store_result_view(const ayni_body_store_v1 &, uint64_t scope,
                                const ayni_task_call_result_v1 &, BodyRef &);
// references selects index-only collection; the normal reader renders the original saved Schema.
ayni_status_t load_result_view(const ayni_body_store_v1 &, const BodyRef &, std::string &,
                               std::vector<BodyRef> *references);
ayni_status_t load_result_view(BodyReader &, const BodyRef &, std::string &,
                               std::vector<BodyRef> *references);
} // namespace ayni::strategies::detail
#endif
