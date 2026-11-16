// Derived JSON view references the original normalized Core result and its saved Schema.
#ifndef AYNI_STRATEGIES_HISTORY_RESULT_H
#define AYNI_STRATEGIES_HISTORY_RESULT_H
#include "support.h"
namespace ayni::strategies::detail {
ayni_status_t store_result_view(const ayni_body_store_v1 &, uint64_t scope,
                                const ayni_task_call_result_v1 &, BodyRef &,
                                ayni_string_v1 presentation = {});
// Stores a summary presentation over the same original references, preserving their lifetime.
ayni_status_t store_result_presentation(const ayni_body_store_v1 &, uint64_t scope,
                                        const BodyRef &source, ayni_string_v1 presentation,
                                        BodyRef &);
// references selects index-only collection; the normal reader renders the original saved Schema.
ayni_status_t load_result_view(const ayni_body_store_v1 &, const BodyRef &, std::string &,
                               std::vector<BodyRef> *references,
                               const ayni_task_call_result_v1 *source = nullptr);
ayni_status_t load_result_view(BodyReader &, const BodyRef &, std::string &,
                               std::vector<BodyRef> *references,
                               const ayni_task_call_result_v1 *source = nullptr);
} // namespace ayni::strategies::detail
#endif
