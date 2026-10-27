#ifndef AYNI_TESTS_UNIT_GOAL_APPROVAL_REQUEST_H
#define AYNI_TESTS_UNIT_GOAL_APPROVAL_REQUEST_H

#include <ayni/authority.h>
#include <gtest/gtest.h>
#include <vector>

namespace ayni::test {

// ADR-0037: construct management's echo using only copying public queries.
// Own the strings/parameters so the request remains valid after Goal reclamation.
struct ApprovalRequest {
    ayni_goal_info_v1 info{};
    ayni_calendar_info_v1 calendar{}; // Owns the borrowed calendar echo through the request.
    std::vector<uint8_t> parameters;
    ayni_goal_approve_v1 body{};

    ApprovalRequest(ayni_kernel_t *kernel, uint64_t goal_id) {
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal(kernel, goal_id, &info), AYNI_OK);
        uint32_t size = 0;
        const auto measured = ayni_get_goal_parameters(kernel, goal_id, nullptr, 0, &size);
        EXPECT_EQ(measured, size == 0U ? AYNI_OK : AYNI_BUFFER_TOO_SMALL);
        parameters.resize(size);
        EXPECT_EQ(ayni_get_goal_parameters(kernel, goal_id, parameters.data(), size, &size),
                  AYNI_OK);
        body.struct_size = sizeof(body);
        body.goal_id = goal_id;
        body.lifetime = AYNI_GRANT_PERMANENT;
        auto &original = body.original;
        original.struct_size = sizeof(original);
        original.owner_instance_id = info.owner_instance_id;
        original.parent_goal_id = info.parent_goal_id;
        original.goal_type_id = {info.goal_type_id, info.goal_type_id_size};
        original.goal_type_version = info.goal_type_version;
        original.parameters = {parameters.data(), size};
        original.trigger = info.trigger;
        if (info.trigger == AYNI_TRIGGER_CALENDAR_ONCE ||
            info.trigger == AYNI_TRIGGER_CALENDAR_DAILY ||
            info.trigger == AYNI_TRIGGER_CALENDAR_WEEKLY) {
            calendar.struct_size = sizeof(calendar);
            EXPECT_EQ(ayni_get_calendar(kernel, goal_id, &calendar), AYNI_OK);
            original.calendar = &calendar.rule;
        }
        original.trigger_delay_us = info.trigger_delay_us;
        original.trigger_period_us = info.trigger_period_us;
        original.expiry_us = info.original_expiry_us;
        original.trigger_source = {info.trigger_source, info.trigger_source_size};
        ayni_goal_authority_info_v1 authority{};
        authority.struct_size = sizeof(authority);
        EXPECT_EQ(ayni_get_goal_authority(kernel, goal_id, &authority), AYNI_OK);
        for (uint32_t index = 0; index < authority.requirement_count; ++index) {
            ayni_goal_requirement_info_v1 need{};
            need.struct_size = sizeof(need);
            EXPECT_EQ(ayni_get_goal_requirement(kernel, goal_id, index, &need), AYNI_OK);
            if (need.grant_id == 0U) {
                body.missing_mask |= uint32_t{1} << index;
            }
        }
    }
    ApprovalRequest(const ApprovalRequest &) = delete;
    ApprovalRequest &operator=(const ApprovalRequest &) = delete;
};

} // namespace ayni::test
#endif
