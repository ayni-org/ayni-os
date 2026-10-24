#ifndef AYNI_TESTS_UNIT_GOAL_REAUTHORIZATION_REQUEST_H
#define AYNI_TESTS_UNIT_GOAL_REAUTHORIZATION_REQUEST_H

#include "goal_approval_request.h"

namespace ayni::test {
// ADR-0038: keep the complete caller-owned echo across retries and reclamation.
struct ReauthorizationRequest {
    ApprovalRequest echo;
    ayni_goal_reauthorize_v1 body{};
    ReauthorizationRequest(ayni_kernel_t *kernel, uint64_t goal_id) : echo(kernel, goal_id) {
        ayni_goal_authority_info_v1 authority{};
        authority.struct_size = sizeof(authority);
        EXPECT_EQ(ayni_get_goal_authority(kernel, goal_id, &authority), AYNI_OK);
        body.struct_size = sizeof(body);
        body.goal_id = goal_id;
        body.expected_authority_commit_seq = authority.commit_seq;
        body.original = echo.body.original;
    }
};
} // namespace ayni::test
#endif
