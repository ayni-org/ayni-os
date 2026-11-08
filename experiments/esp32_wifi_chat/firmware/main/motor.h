#pragma once
#include "motor_execution.h"
#include <array>
#include <ayni/calls.h>
namespace wifichat {
// Owns copied tool descriptors and the one local physical executor. Core owns Calls.
class Motor {
  public:
    explicit Motor(Transport &transport) : transport_(transport), execution_(transport) {}
    MotorExecution &execution() { return execution_; }
    ayni_status_t configure(ayni_kernel_t *);

  private:
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
    Transport &transport_;
    MotorExecution execution_;
    std::array<ayni_schema_node_v1, 4> plan_input_{}, run_input_{};
    std::array<ayni_authority_projection_v1, 3> projections_{};
    ayni_call_resource_v1 resource_{};
    ayni_schema_node_v1 status_input_{}, result_{};
    std::array<ayni_business_contract_v1, 4> business_{};
    std::array<ayni_call_capability_v1, 4> offers_{};
    ayni_provider_v1 provider_{};
};
} // namespace wifichat
