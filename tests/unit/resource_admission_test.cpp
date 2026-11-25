// White-box resource mechanism scenarios. Calls/permits consume this module
// in MA-008/018B; these tests do not claim durable dispatch or physical stop.
#include "resources/admission/resource_admission.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace {
using namespace ayni::resources;
constexpr uint32_t resource_count = 4;
constexpr uint32_t request_count = 16;

struct Arena {
    explicit Arena(std::size_t size) : bytes((size + sizeof(uint64_t) - 1) / sizeof(uint64_t)) {}
    std::vector<uint64_t> bytes;
    void *data() { return bytes.data(); }
};

class ResourceAdmissionTest : public ::testing::Test {
  protected:
    explicit ResourceAdmissionTest(bool conflicts = false)
        : catalog_memory(ayni::catalog::ResourceRegistry::storage_size(resource_count)),
          state_memory(ResourceAdmission::state_storage_size(resource_count)),
          request_memory(ResourceAdmission::request_storage_size(request_count)),
          registry(catalog_memory.data(), resource_count),
          admission(registry, state_memory.data(), resource_count, request_memory.data(),
                    request_count) {
        add("model", 5, AYNI_RESOURCE_SHARED, 1);
        add("audio", 5, AYNI_RESOURCE_SHARED, 2, 0, conflicts);
        add("motor", 5, AYNI_RESOURCE_EXCLUSIVE, 1, AYNI_RESOURCE_ACTUATOR, conflicts);
        add("network", 7, AYNI_RESOURCE_SHARED, UINT32_MAX);
    }
    void add(const char *name, uint32_t length, ayni_resource_kind_t kind, uint32_t capacity,
             uint32_t flags = 0, bool conflict = false) {
        const ayni_resource_desc_v1 input{sizeof(input),
                                          kind,
                                          {name, length},
                                          capacity,
                                          flags,
                                          0,
                                          0,
                                          nullptr,
                                          conflict ? ayni_string_v1{"shared.bus", 10}
                                                   : ayni_string_v1{}};
        ayni::catalog::Resource decoded{};
        ASSERT_EQ(registry.decode(input, decoded), AYNI_OK);
        ASSERT_EQ(registry.add_validated(decoded), AYNI_OK);
        bool changed = false;
        ASSERT_EQ(admission.set_health(registry.size() - 1U, AYNI_RESOURCE_ONLINE, changed),
                  AYNI_OK);
    }
    ResourceRequest request(uint64_t call, uint64_t agent, uint32_t resource = 0,
                            uint32_t units = 1) {
        ResourceRequest result{};
        result.call_id = call;
        result.instance_id = agent;
        result.priority = Priority::normal;
        result.on_wait = WaitAction::wait;
        result.wait_us = no_deadline;
        result.count = 1;
        result.resources[0] = {resource, units};
        return result;
    }
    void queue(const ResourceRequest &value, uint64_t now = 0) {
        ASSERT_EQ(admission.enqueue(value, now), AYNI_OK);
    }
    uint64_t next(uint64_t now = 0) {
        AdmissionChoice choice{};
        EXPECT_TRUE(admission.next(now, choice));
        EXPECT_FALSE(choice.expired);
        return choice.call_id;
    }
    void take(uint64_t call, uint64_t now = 0) {
        ASSERT_EQ(next(now), call);
        ASSERT_TRUE(acquire(call, now));
    }
    bool acquire(uint64_t call, uint64_t now) {
        ResourceClaim claim{};
        auto on_wait = WaitAction::wait;
        const bool accepted = admission.acquire(call, now, claim, on_wait) ==
                              ResourceAdmission::Acquisition::acquired;
        if (accepted) {
            claims[call] = claim;
        }
        return accepted;
    }
    bool release(uint64_t call) {
        const auto found = claims.find(call);
        return admission.release(found == claims.end() ? ResourceClaim{call, 0} : found->second);
    }
    std::map<uint64_t, ResourceClaim> claims;
    Arena catalog_memory;
    Arena state_memory;
    Arena request_memory;
    ayni::catalog::ResourceRegistry registry;
    ResourceAdmission admission;
};

// C-RES-004: storage pressure skips a blocked Agent, preserving its FIFO.
TEST_F(ResourceAdmissionTest, ram_only_admission_keeps_per_agent_fifo) {
    auto durable = request(1, 1);
    durable.on_wait = WaitAction::pause;
    durable.wait_us = 30;
    queue(durable);
    auto same_agent = request(2, 1);
    same_agent.lane = ResourceRequest::Lane::ephemeral;
    queue(same_agent);
    auto other_agent = request(3, 2, 1);
    other_agent.lane = ResourceRequest::Lane::ephemeral;
    other_agent.on_wait = WaitAction::pause;
    other_agent.wait_us = 60;
    queue(other_agent);

    AdmissionChoice choice{};
    auto on_wait = WaitAction::wait;
    ResourceClaim claim{};
    using Admission = ResourceAdmission::Admission;
    EXPECT_EQ(admission.next_deadline_us(), durable.wait_us);
    EXPECT_EQ(admission.next_deadline_us(Admission::ram_only), other_agent.wait_us);
    ASSERT_TRUE(admission.next(0, choice, Admission::ram_only));
    EXPECT_EQ(choice.call_id, other_agent.call_id);
    EXPECT_EQ(admission.acquire(other_agent.call_id, 0, claim, on_wait, Admission::ram_only),
              ResourceAdmission::Acquisition::acquired);
    EXPECT_FALSE(admission.next(0, choice, Admission::ram_only));
    take(1);
    ASSERT_TRUE(release(1));
    take(2);
}

// C-RES-002/003: unavailable groups never consume a subset of units.
TEST_F(ResourceAdmissionTest, group_admission_is_atomic_and_held_units_do_not_overflow) {
    queue(request(1, 1, 1, 2));
    take(1);
    auto group = request(2, 2);
    group.count = 2;
    group.resources[1] = {1, 1};
    queue(group);
    EXPECT_FALSE(acquire(2, 0));
    EXPECT_EQ(admission.info(0).used, 0U);
    EXPECT_EQ(admission.info(1).used, 2U);
    queue(request(3, 3));
    take(3);
    ASSERT_TRUE(release(3));
    ASSERT_TRUE(release(1));
    take(2);
    EXPECT_EQ(admission.info(0).used, 1U);
    EXPECT_EQ(admission.info(1).used, 1U);

    queue(request(4, 4, 3, UINT32_MAX));
    take(4);
    queue(request(5, 5, 3));
    EXPECT_FALSE(acquire(5, 0));
    EXPECT_EQ(admission.info(3).used, UINT32_MAX);
    ASSERT_TRUE(release(4));
    take(5);
}

// C-RES-004, ADR-0042: repeated traffic from one Agent does not get extra turns.
TEST_F(ResourceAdmissionTest, agents_rotate_and_each_agents_requests_remain_fifo) {
    queue(request(1, 10));
    queue(request(2, 10));
    queue(request(3, 10));
    queue(request(4, 20));
    queue(request(5, 20));
    queue(request(6, 30));
    for (const auto call : {1U, 4U, 6U, 2U, 5U, 3U}) {
        take(call);
        ASSERT_TRUE(release(call));
    }
}

// C-RES-004/011: a blocked Agent head blocks its own tail, not another Agent.
TEST_F(ResourceAdmissionTest, blocked_head_preserves_fifo_and_other_agents_can_use_free_resources) {
    queue(request(1, 1));
    take(1);
    queue(request(2, 10));
    queue(request(3, 10, 1));
    queue(request(4, 20, 1));
    take(4);
    EXPECT_FALSE(acquire(3, 0));
    WaitReason reason{};
    uint32_t resource = 0;
    uint64_t waited = 0;
    ASSERT_TRUE(admission.waiting(3, 8, reason, resource, waited));
    EXPECT_EQ(reason, WaitReason::fifo);
    EXPECT_EQ(resource, ResourceAdmission::no_index);
    EXPECT_EQ(waited, 8U);
    ASSERT_TRUE(release(1));
    take(2);
    take(3);
}

// C-RES-004/006: priority affects only admission, never a running holder.
TEST_F(ResourceAdmissionTest, foreground_goes_first_without_preempting_existing_background_work) {
    auto background = request(1, 1);
    background.priority = Priority::background;
    queue(background);
    take(1);
    queue(request(2, 2));
    auto foreground = request(3, 3);
    foreground.priority = Priority::foreground;
    queue(foreground);
    EXPECT_FALSE(acquire(3, 0));
    EXPECT_EQ(admission.info(0).used, 1U);
    ASSERT_TRUE(release(1));
    EXPECT_FALSE(acquire(2, 0));
    take(3);
    ASSERT_TRUE(release(3));
    take(2);
}

// C-RES-005: finite waits are returned for explicit work-owner handling; no
// silent removal or conversion to an unlimited wait occurs near clock limits.
TEST_F(ResourceAdmissionTest,
       expired_waits_report_action_before_acquisition_and_reminders_can_wait) {
    auto finite = request(1, 1);
    finite.wait_us = 20;
    finite.on_wait = WaitAction::pause;
    queue(finite, 10);
    queue(request(2, 2), 10);
    EXPECT_EQ(admission.next_deadline_us(), 30U);
    AdmissionChoice selected{};
    ASSERT_TRUE(admission.next(30, selected));
    EXPECT_TRUE(selected.expired);
    EXPECT_EQ(selected.on_wait, WaitAction::pause);
    EXPECT_EQ(selected.waited_us, 20U);
    ResourceClaim untouched{99, 1};
    auto on_wait = WaitAction::wait;
    const auto before = admission.info(0).used;
    EXPECT_EQ(admission.acquire(2, 30, untouched, on_wait),
              ResourceAdmission::Acquisition::not_selected);
    EXPECT_EQ(on_wait, WaitAction::wait);
    EXPECT_EQ(admission.acquire(1, 30, untouched, on_wait),
              ResourceAdmission::Acquisition::expired);
    EXPECT_EQ(on_wait, WaitAction::pause);
    EXPECT_EQ(untouched.call_id, 99U);
    EXPECT_EQ(untouched.generation, 1U);
    EXPECT_EQ(admission.info(0).used, before);
    EXPECT_EQ(admission.size(), 2U);
    ASSERT_TRUE(admission.remove_waiter(1, 30));
    EXPECT_EQ(admission.next_deadline_us(), no_deadline);
    take(2, 100);
    EXPECT_EQ(admission.longest_wait_us(), 90U);
    ASSERT_TRUE(release(2));
    finite.call_id = 3;
    finite.on_wait = WaitAction::skip;
    queue(finite, no_deadline - 10U);
    EXPECT_EQ(admission.next_deadline_us(), no_deadline - 1U);
    ASSERT_TRUE(admission.next(no_deadline - 1U, selected));
    EXPECT_TRUE(selected.expired);
    EXPECT_EQ(selected.on_wait, WaitAction::skip);
}

// C-RES-008: release and health are not a proof of stop; stale confirmations
// must not unlock a resource held/quarantined by a different logical call.
TEST_F(ResourceAdmissionTest, actuator_quarantine_requires_the_exact_released_call_confirmation) {
    queue(request(1, 1, 2));
    take(1);
    EXPECT_FALSE(admission.confirm_stopped(2, claims.at(1)));
    ASSERT_TRUE(release(1));
    EXPECT_EQ(admission.info(2).state, AYNI_RESOURCE_QUARANTINED);
    EXPECT_EQ(admission.info(2).used, 0U);
    bool changed = false;
    ASSERT_EQ(admission.set_health(2, AYNI_RESOURCE_OFFLINE, changed), AYNI_OK);
    EXPECT_EQ(admission.info(2).quarantine_id, 1U);
    ASSERT_EQ(admission.set_health(2, AYNI_RESOURCE_ONLINE, changed), AYNI_OK);
    queue(request(2, 2, 2));
    EXPECT_FALSE(acquire(2, 0));
    EXPECT_FALSE(admission.confirm_stopped(2, {99, 1}));
    ASSERT_TRUE(admission.confirm_stopped(2, claims.at(1)));
    take(2);
    ASSERT_TRUE(release(2));
    EXPECT_FALSE(admission.confirm_stopped(2, claims.at(1)));
    EXPECT_EQ(admission.info(2).quarantine_id, 2U);
    ASSERT_TRUE(admission.confirm_stopped(2, claims.at(2)));
    EXPECT_EQ(admission.info(2).state, AYNI_RESOURCE_FREE);
}

// C-RES-008, ADR-0042: reacquiring a logical call is a new occupancy, so a
// delayed release or stop confirmation from the first occupancy is stale.
TEST_F(ResourceAdmissionTest, old_claim_cannot_release_or_unquarantine_a_reacquired_call) {
    queue(request(1, 1, 2));
    take(1);
    const auto first = claims.at(1);
    ASSERT_TRUE(release(1));
    ASSERT_TRUE(admission.confirm_stopped(2, first));
    queue(request(1, 1, 2));
    take(1);
    EXPECT_NE(claims.at(1).generation, first.generation);
    EXPECT_FALSE(admission.release(first));
    EXPECT_EQ(admission.info(2).used, 1U);
    ASSERT_TRUE(release(1));
    EXPECT_FALSE(admission.confirm_stopped(2, first));
    EXPECT_EQ(admission.info(2).state, AYNI_RESOURCE_QUARANTINED);
    EXPECT_EQ(admission.info(2).quarantine_generation, claims.at(1).generation);
    ASSERT_TRUE(admission.confirm_stopped(2, claims.at(1)));
}

// C-RES-009/013: health changes neither lose live ownership nor overallocate.
TEST_F(ResourceAdmissionTest, health_does_not_clear_holders_and_shared_spare_units_remain_usable) {
    queue(request(1, 1, 1));
    take(1);
    queue(request(2, 2, 1));
    take(2);
    bool changed = false;
    ASSERT_EQ(admission.set_health(1, AYNI_RESOURCE_FAULTED, changed), AYNI_OK);
    EXPECT_EQ(admission.info(1).used, 2U);
    EXPECT_EQ(admission.info(1).state, AYNI_RESOURCE_UNAVAILABLE);
    ASSERT_TRUE(release(1));
    queue(request(3, 3, 1));
    EXPECT_FALSE(acquire(3, 0));
    ASSERT_EQ(admission.set_health(1, AYNI_RESOURCE_ONLINE, changed), AYNI_OK);
    take(3);
    EXPECT_EQ(admission.info(1).used, 2U);
    EXPECT_FALSE(release(3 + 100));
    EXPECT_FALSE(admission.remove_waiter(3, 0));
}

// C-RES-002/004: rejected groups and duplicate identities consume no capacity.
TEST_F(ResourceAdmissionTest, malformed_groups_and_full_queue_preserve_existing_requests) {
    auto bad = request(1, 1);
    bad.count = 2;
    bad.resources[1] = bad.resources[0];
    EXPECT_EQ(admission.enqueue(bad, 0), AYNI_INVALID_ARGUMENT);
    bad = request(1, 1, 0, 2);
    EXPECT_EQ(admission.enqueue(bad, 0), AYNI_INVALID_ARGUMENT);
    bad = request(1, 1, resource_count);
    EXPECT_EQ(admission.enqueue(bad, 0), AYNI_INVALID_ARGUMENT);
    bad = request(1, 1);
    bad.on_wait = WaitAction::pause;
    EXPECT_EQ(admission.enqueue(bad, 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(admission.size(), 0U);
    for (uint64_t call = 1; call <= request_count; ++call) {
        queue(request(call, call));
    }
    EXPECT_EQ(admission.enqueue(request(1, 1), 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(admission.enqueue(request(request_count + 1U, 1), 0), AYNI_CAPACITY_EXCEEDED);
    take(1);
    EXPECT_EQ(admission.size(), request_count);
    ASSERT_TRUE(release(1));
    queue(request(request_count + 1U, 1));
    take(2);
}

class ConflictDomainTest : public ResourceAdmissionTest {
  protected:
    ConflictDomainTest() : ResourceAdmissionTest(true) {}
};

// C-RES-002/004: same-resource sharing survives, conflicting resources wait atomically.
TEST_F(ConflictDomainTest, sharing_and_unrelated_agents_continue_while_other_domain_member_waits) {
    queue(request(1, 1, 1));
    take(1);
    queue(request(2, 2, 1));
    take(2);
    queue(request(3, 3, 2));
    EXPECT_FALSE(acquire(3, 0));
    WaitReason reason{};
    uint32_t resource = 0;
    uint64_t waited = 0;
    ASSERT_TRUE(admission.waiting(3, 0, reason, resource, waited));
    EXPECT_EQ(reason, WaitReason::conflict);
    EXPECT_EQ(resource, 1U);
    queue(request(4, 4, 0));
    take(4);
    EXPECT_EQ(admission.info(2).used, 0U);
    ASSERT_TRUE(release(1));
    EXPECT_FALSE(acquire(3, 0));
    ASSERT_TRUE(release(2));
    take(3);
}

// C-RES-008: health cannot release another member's unconfirmed stop isolation.
TEST_F(ConflictDomainTest, stop_quarantine_blocks_all_other_members_until_exact_confirmation) {
    queue(request(1, 1, 2));
    take(1);
    ASSERT_TRUE(release(1));
    queue(request(2, 2, 1));
    EXPECT_FALSE(acquire(2, 0));
    bool changed = false;
    ASSERT_EQ(admission.set_health(2, AYNI_RESOURCE_OFFLINE, changed), AYNI_OK);
    EXPECT_FALSE(acquire(2, 0));
    EXPECT_FALSE(admission.confirm_stopped(2, {1, claims[1].generation + 1}));
    ASSERT_TRUE(admission.confirm_stopped(2, claims[1]));
    take(2);
}

// C-RES-002: an impossible group cannot consume a queue slot or a resource unit.
TEST_F(ConflictDomainTest, self_conflicting_group_is_rejected_without_partial_admission) {
    auto group = request(1, 1, 1);
    group.count = 2;
    group.resources[1] = {2, 1};
    EXPECT_EQ(admission.enqueue(group, 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(admission.size(), 0U);
    EXPECT_EQ(admission.info(1).used, 0U);
    EXPECT_EQ(admission.info(2).used, 0U);
}

// ADR-0111: conflicting_holders reports direct and conflict-domain holders and unavailable gates.
TEST_F(ConflictDomainTest, conflicting_holders_reports_direct_domain_and_unavailable) {
    auto invalid = request(99, 1, 2);
    invalid.preemption = AYNI_CALL_PREEMPT_SUPERSEDE + 1U;
    EXPECT_EQ(admission.enqueue(invalid, 0), AYNI_INVALID_ARGUMENT);

    // Call 1 holds shared resource 1 (in shared.bus conflict domain with motor resource 2).
    queue(request(1, 1, 1));
    take(1);
    auto waiter = request(2, 2, 2);
    waiter.preemption = AYNI_CALL_PREEMPT_SUPERSEDE;
    queue(waiter);
    std::array<HeldConflict, 4> conflicts{};
    bool unavailable = false;
    uint64_t waiter_ticket = 0;
    Priority waiter_priority = Priority::background;
    uint32_t count = admission.conflicting_holders(2, conflicts.data(), conflicts.size(),
                                                   unavailable, waiter_ticket, waiter_priority);
    EXPECT_FALSE(unavailable);
    EXPECT_EQ(waiter_priority, Priority::normal);
    EXPECT_GT(waiter_ticket, 0U);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(conflicts[0].call_id, 1U);
    EXPECT_EQ(conflicts[0].priority, Priority::normal);
    EXPECT_LT(conflicts[0].ticket, waiter_ticket);

    // Quarantine on motor resource 2 blocks acquisition as unavailable (not preemptable).
    ASSERT_TRUE(release(1));
    take(2);
    ASSERT_TRUE(release(2));
    auto waiter3 = request(3, 3, 1);
    waiter3.preemption = AYNI_CALL_PREEMPT_SUPERSEDE;
    queue(waiter3);
    unavailable = false;
    count = admission.conflicting_holders(3, conflicts.data(), conflicts.size(), unavailable,
                                          waiter_ticket, waiter_priority);
    EXPECT_TRUE(unavailable);
    EXPECT_EQ(count, 0U);
}

// ADR-0111: an eligible preempting waiter is selected ahead of an older ordinary same-priority
// waiter.
TEST_F(ResourceAdmissionTest,
       preempting_waiter_is_selected_ahead_of_ordinary_same_priority_waiter) {
    queue(request(1, 1, 0));
    take(1);
    queue(request(2, 2, 0));
    auto preemptor = request(3, 3, 0);
    preemptor.preemption = AYNI_CALL_PREEMPT_SUPERSEDE;
    queue(preemptor);
    ASSERT_TRUE(release(1));
    // Preemptor (call 3) takes the freed slot ahead of ordinary waiter (call 2).
    take(3);
    ASSERT_TRUE(release(3));
    take(2);
}

// ADR-0111: visit_conflicting_holders() streams more than AYNI_RESOURCE_GROUP_COUNT_MAX (8)
// shared-resource holders without truncating or overflowing a small conflict buffer.
TEST(ResourceAdmissionSharedPreemption,
     visit_conflicting_holders_handles_more_than_eight_shared_holders) {
    constexpr uint32_t res_cap = 1;
    constexpr uint32_t req_cap = 16;
    Arena cat_mem(ayni::catalog::ResourceRegistry::storage_size(res_cap));
    Arena st_mem(ResourceAdmission::state_storage_size(res_cap));
    Arena req_mem(ResourceAdmission::request_storage_size(req_cap));
    ayni::catalog::ResourceRegistry reg(cat_mem.data(), res_cap);
    ResourceAdmission adm(reg, st_mem.data(), res_cap, req_mem.data(), req_cap);
    const ayni_resource_desc_v1 desc{
        sizeof(desc), AYNI_RESOURCE_SHARED, {"workers", 7}, 12, 0, 0, 0, nullptr, {}};
    ayni::catalog::Resource decoded{};
    ASSERT_EQ(reg.decode(desc, decoded), AYNI_OK);
    ASSERT_EQ(reg.add_validated(decoded), AYNI_OK);
    bool changed = false;
    ASSERT_EQ(adm.set_health(0, AYNI_RESOURCE_ONLINE, changed), AYNI_OK);

    // Hold 10 concurrent background calls (> AYNI_RESOURCE_GROUP_COUNT_MAX == 8), 1 unit each.
    for (uint64_t id = 1; id <= 10; ++id) {
        ResourceRequest r{};
        r.call_id = id;
        r.instance_id = id;
        r.priority = Priority::background;
        r.on_wait = WaitAction::wait;
        r.wait_us = no_deadline;
        r.count = 1;
        r.resources[0] = {0, 1};
        ASSERT_EQ(adm.enqueue(r, 0), AYNI_OK);
        AdmissionChoice choice{};
        ASSERT_TRUE(adm.next(0, choice));
        ASSERT_EQ(choice.call_id, id);
        ResourceClaim claim{};
        WaitAction on_wait{};
        ASSERT_EQ(adm.acquire(id, 0, claim, on_wait), ResourceAdmission::Acquisition::acquired);
    }

    // Queue a foreground preemptor needing 6 units (blocked by all 10 holders).
    ResourceRequest waiter{};
    waiter.call_id = 100;
    waiter.instance_id = 100;
    waiter.priority = Priority::foreground;
    waiter.on_wait = WaitAction::wait;
    waiter.wait_us = no_deadline;
    waiter.count = 1;
    waiter.resources[0] = {0, 6};
    waiter.preemption = AYNI_CALL_PREEMPT_SUPERSEDE;
    ASSERT_EQ(adm.enqueue(waiter, 10), AYNI_OK);

    bool unavailable = false;
    uint64_t waiter_ticket = 0;
    Priority waiter_priority = Priority::background;
    std::vector<uint64_t> seen;
    const uint32_t total = adm.visit_conflicting_holders(
        100, unavailable, waiter_ticket, waiter_priority, &seen,
        [](void *ctx, const HeldConflict &c) {
            static_cast<std::vector<uint64_t> *>(ctx)->push_back(c.call_id);
            return true;
        });
    EXPECT_FALSE(unavailable);
    EXPECT_EQ(total, 10U);
    EXPECT_EQ(seen.size(), 10U);
}
} // namespace
