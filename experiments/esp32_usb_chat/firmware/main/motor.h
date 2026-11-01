#pragma once
#include <array>
#include <ayni/calls.h>
namespace usbchat {
// Software-stage TA6586 inventory and planning only. Owns copied-registration
// backing descriptors; has no GPIO/PWM, executor, permit or enabled motion path.
class Motor {
  public:
    ayni_status_t configure(ayni_kernel_t *);
    ayni_schema_v1 schema(ayni_string_v1 binding) const;

  private:
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    std::array<ayni_schema_node_v1, 4> plan_input_{};
    ayni_schema_node_v1 status_input_{}, result_{};
    std::array<ayni_business_contract_v1, 2> business_{};
    std::array<ayni_call_capability_v1, 2> offers_{};
    ayni_provider_v1 provider_{};
};
} // namespace usbchat
