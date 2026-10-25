#ifndef AYNI_CORE_RESOURCES_ADMISSION_RESOURCE_ADMISSION_H
#define AYNI_CORE_RESOURCES_ADMISSION_RESOURCE_ADMISSION_H

#include "catalog/resource/resource_registry.h"
#include "foundation/bounded_array.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::resources {

// System stop bypasses this queue. Priorities never preempt existing holders.
enum class Priority : uint32_t { foreground, normal, background, count };
// Outcome for the work owner when a queued request reaches its deadline.
enum class WaitAction : uint32_t { pause, skip, wait };
// Pending diagnostics; available is neither an acquisition nor authorization.
enum class WaitReason { available, fifo, occupied, offline, faulted, quarantined, conflict };

using catalog::resource_group_max;
constexpr uint64_t no_deadline = UINT64_MAX;

// One resolved immutable catalog entry, in strictly increasing index order.
struct ResourceUnits {
    uint32_t index; // Registry slot, valid even for slot 0.
    uint32_t units; // 1..registered capacity; never zero.
};

// Borrowed only for enqueue; the queue copies the complete bounded request.
struct ResourceRequest {
    uint64_t call_id;     // Nonzero stable logical call, unique while retained.
    uint64_t instance_id; // Nonzero owner for fairness; never changed by a retry.
    Priority priority;    // One of the three queued priorities, not count.
    WaitAction on_wait;   // wait requires no_deadline; pause/skip require finite timeout.
    uint64_t wait_us;     // Relative monotonic duration; 0 may expire immediately.
    uint32_t count;       // 0..resource_group_max; zero-resource calls still obey FIFO.
    std::array<ResourceUnits, resource_group_max> resources; // Only [0,count) is read.
};

// Selection is an owner-thread snapshot; commit must reselect before acquire.
struct AdmissionChoice {
    uint64_t call_id;   // Selected stable ID, never a mutable queue index.
    uint64_t waited_us; // Nonnegative wait in this boot.
    WaitAction on_wait; // Applied by work owner only when expired is true.
    bool expired;       // True consumes no resource and must be removed explicitly.
};

// One acquisition, not just one logical call: retries may reacquire resources.
struct ResourceClaim {
    uint64_t call_id = 0;    // 0 iff generation is 0 (empty); otherwise a logical call ID.
    uint64_t generation = 0; // Positive enqueue ticket; never wraps within this owner lifetime.
};

// Runtime state has one owner and no persisted monotonic times or occupancy.
// The registry may grow in CONFIGURING, up to the capacity passed here.
class ResourceAdmission {
  public:
    // Align both live resource states and requests in caller-provided blocks.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t state_storage_size(uint32_t resource_capacity);
    static std::size_t request_storage_size(uint32_t request_capacity);
    // Blocks outlive this object; nullptr is allowed only for zero capacity.
    ResourceAdmission(const catalog::ResourceRegistry &registry, void *states,
                      uint32_t resource_capacity, void *requests, uint32_t request_capacity);
    // Changes availability only; cannot free a claim or clear quarantine.
    ayni_status_t set_health(uint32_t index, ayni_resource_health_t health, bool &changed);
    // Copied runtime info; index < registry size. No borrowed pointers.
    ayni_resource_info_v1 info(uint32_t index) const;
    // Admission capacity only; no reservation or fairness change. Descriptor
    // validation remains in enqueue, after Kernel has preflighted the other owners.
    bool has_room() const { return !requests_.full() && last_ticket_ != UINT64_MAX; }
    // Validates the entire group before copying; failure has no side effects.
    ayni_status_t enqueue(const ResourceRequest &request, uint64_t now_us);
    // Expired requests first, then highest-priority eligible Agent in a ring
    // ordered by instance ID; its oldest request alone may be selected.
    bool next(uint64_t now_us, AdmissionChoice &out) const;
    // Reselects under the same owner before acquiring all units atomically;
    // false preserves out and changes nothing (including fairness) if selection
    // has changed. Success returns the exact acquisition identity.
    bool acquire(uint64_t call_id, uint64_t now_us, ResourceClaim &out);
    // Removes a queued request (cancel/expiry), never an acquired request.
    bool remove_waiter(uint64_t call_id, uint64_t now_us);
    // Final dispatch check: this exact held generation still exists and every
    // resource remains online and outside quarantine. Does not reacquire units.
    bool executable(ResourceClaim claim) const;
    // Releases a claim. Actuators remain unavailable pending exact stop proof;
    // ordinary resources release even when remote completion is unknown.
    bool release(ResourceClaim claim);
    // Confirms this exact actuator claim after release; cannot release a newer
    // claim, including a newer acquisition of the same call. Only the trusted
    // execution consumer may call, not a health report.
    bool confirm_stopped(uint32_t resource_index, ResourceClaim claim);
    // Same trusted proof applies to every actuator of this exact atomic group.
    void confirm_all_stopped(ResourceClaim claim);
    // Why this queued request cannot acquire now; no_index when no resource
    // is responsible (FIFO/available). Returns false for absent/held calls.
    bool waiting(uint64_t call_id, uint64_t now_us, WaitReason &reason, uint32_t &resource_index,
                 uint64_t &waited_us) const;
    // Earliest pending finite wait deadline; no_deadline if none.
    uint64_t next_deadline_us() const;
    // Boot-local peak wait among requests acquired or removed.
    uint64_t longest_wait_us() const { return longest_wait_us_; }
    // Copy group counts and maximum current/completed wait, without mutating statistics.
    ayni_resource_queue_info_v1 queue_info(uint64_t now_us) const;
    // Number of live queued and held requests; no hidden overflow allocation.
    uint32_t size() const { return requests_.size(); }
    static constexpr uint32_t no_index = UINT32_MAX;

  private:
    // Physical availability and occupancy are independent. Health cannot prove stop.
    struct State {
        ayni_resource_health_t health = AYNI_RESOURCE_OFFLINE; // Latest Host fact.
        uint32_t used = 0;                                     // Units of active claims only.
        ResourceClaim quarantine; // Exact released claim; empty iff no unconfirmed stop.
    };
    enum class Phase { waiting, held };
    // Queue record keeps the request through acquisition until explicit release.
    struct Entry {
        ResourceRequest request; // Owned copy; immutable until removed.
        uint64_t ticket;         // Positive enqueue sequence; never wraps/reused.
        uint64_t queued_us;      // Monotonic enqueue instant, can be zero.
        uint64_t deadline_us;    // Absolute boot-local deadline; no_deadline for wait.
        Phase phase;             // Determines whether it owns units or waits.
    };
    // Closed descriptor validation; checks catalog, canonical group and bounds.
    ayni_status_t validate(const ResourceRequest &request) const;
    // Finds by stable ID; false leaves index intact.
    bool find(uint64_t call_id, uint32_t &index) const;
    // Oldest waiter of this Agent at this priority; higher priorities independent.
    bool head(uint32_t index) const;
    // Whole-group preflight; available only when every resource has room.
    WaitReason availability(const Entry &entry, uint32_t &resource_index) const;
    // Least enqueue ticket of all expired waiters, no FIFO blocking on expiry.
    uint32_t expired(uint64_t now_us) const;
    // Fair work-conserving selection among eligible heads of one priority.
    uint32_t eligible(Priority priority) const;
    // Capture an immutable candidate without exposing its storage slot.
    AdmissionChoice choice(uint32_t index, uint64_t now_us) const;

    const catalog::ResourceRegistry &registry_; // Borrowed static catalog, outlives admission.
    State *states_; // One per catalog capacity; never a second descriptor copy.
    foundation::BoundedArray<Entry> requests_; // Sole owner of wait and hold facts.
    // Last served instance per priority; 0 means no instance served yet.
    std::array<uint64_t, static_cast<std::size_t>(Priority::count)> last_instance_{};
    uint64_t last_ticket_ = 0;     // 0 before first enqueue; exhaustion rejects new requests.
    uint64_t longest_wait_us_ = 0; // Largest observed admission/expiry delay in microseconds.
};

} // namespace ayni::resources
#endif
