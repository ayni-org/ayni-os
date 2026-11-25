// Static ReflexRule table and bounded asynchronous reflex event ring (ADR-0112).
// Rules register in CONFIGURING and are resolved/validated against catalogs, agents and Scenes
// at ayni_start(). Runtime enable flags, latches and events stay purely in RAM (0 WAL).
#ifndef AYNI_CORE_KERNEL_REFLEX_REFLEX_TABLE_H
#define AYNI_CORE_KERNEL_REFLEX_REFLEX_TABLE_H

#include "authority/scene/scene_table.h"
#include "catalog/catalog_id.h"
#include "catalog/observation/source_registry.h"
#include "foundation/workspace.h"
#include "kernel/config/catalogs.h"
#include "work/agent/agent_table.h"

#include <array>
#include <ayni/reflex.h>
#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

// Complete registered ReflexRule plus boot-local runtime counters/latches.
struct ReflexRuleEntry {
    static constexpr uint32_t no_index = UINT32_MAX; // Sentinel for unresolved or unused index.

    catalog::CatalogId rule_id;                  // Unique nonempty rule identity.
    uint32_t version = 0;                        // Positive semantic rule version.
    ayni_reflex_action_t action = 0;             // SOFT_STOP_RESOURCE or PREEMPT_CALL.
    ayni_reflex_trigger_mode_t trigger_mode = 0; // IN_RANGE or STALE_OR_GAP.
    uint32_t flags = 0;                          // AYNI_REFLEX_FLAG_* bits.
    uint32_t priority = 0;                       // 0..255; higher evaluated first.
    uint64_t cooldown_us = 0;                    // Minimum interval between consecutive fires.
    catalog::CatalogId scene_id;                 // Optional Scene gate; empty disables Scene gate.
    uint32_t scene_version = 0;      // Exact Scene version iff scene_id nonempty, else 0.
    uint32_t scene_index = no_index; // Resolved Scene definition slot, or no_index.
    uint32_t condition_count = 0;    // 1..AYNI_REFLEX_CONDITION_COUNT_MAX.
    std::array<catalog::ObservationCondition, AYNI_REFLEX_CONDITION_COUNT_MAX> conditions{};
    catalog::CatalogId resource_id;     // Required for SOFT_STOP_RESOURCE; empty otherwise.
    uint32_t resource_index = no_index; // Resolved ResourceRegistry slot, or no_index.
    uint64_t owner_agent_id = 0;        // Required agent_id for PREEMPT_CALL; else 0.
    catalog::CatalogId binding_id;      // Required binding_id for PREEMPT_CALL; else empty.
    uint32_t binding_version = 0;       // Positive exact binding version for PREEMPT_CALL.
    uint32_t binding_index = no_index;  // Resolved BindingRegistry slot, or no_index.
    uint32_t input_size = 0;            // Encoded canonical input bytes for PREEMPT_CALL.

    // One immutable mapping paired with its canonical startup-resolved patch location.
    struct Parameter {
        ayni_reflex_param_binding_v1 binding{}; // Copied validated affine/clamp contract.
        uint32_t byte_offset = 0; // Resolved I64 payload offset, valid only after validate_all.
    };
    uint32_t parameter_count = 0; // Active prefix; zero means use original immutable input.
    std::array<Parameter, AYNI_REFLEX_PARAM_BINDING_MAX> parameters{};

    // Boot-local volatile state reset in begin_boot(); never persisted.
    bool enabled = false;        // Current RAM enable gate.
    bool latched_active = false; // True while EDGE_TRIGGERED rule awaits condition reset.
    bool has_fired = false;      // Explicit flag so last_fired_us == 0 is unambiguous at t=0.
    uint64_t last_fired_us = 0;  // Owner monotonic timestamp of last fire/attempt.
    uint64_t fire_count = 0;     // Total accepted fires in this boot.
    uint64_t denied_count = 0;   // Total refused triggers in this boot.
    uint64_t active_call_id = 0; // Live reflex Call ID for PREEMPT_CALL, or 0.
};

// Arena-backed owner of static ReflexRules, pre-encoded inputs, and bounded event ring.
class ReflexTable {
  public:
    static constexpr uint32_t priority_max = 255U; // Maximum allowed priority in reflex.h.
    static constexpr uint32_t allowed_flags =
        AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
    static constexpr std::size_t storage_alignment = alignof(uint64_t);

    static std::size_t rule_storage_size(uint32_t capacity, uint32_t input_bytes);
    static std::size_t event_storage_size(uint32_t event_capacity);

    // Caller arena regions outlive this table; null allowed iff respective capacity is zero.
    ReflexTable(void *rule_storage, uint32_t rule_capacity, uint32_t input_bytes,
                void *event_storage, uint32_t event_capacity);
    ReflexTable(const ReflexTable &) = delete;
    ReflexTable &operator=(const ReflexTable &) = delete;

    // CONFIGURING only: copies and validates rule shape and raw input tree.
    ayni_status_t add_rule(const ayni_reflex_rule_v1 &raw);
    // ayni_start() cross-catalog resolution, MEASURED enforcement, and input normalization.
    ayni_status_t validate_all(foundation::Workspace &workspace, const Catalogs &catalogs,
                               const work::AgentTable &agents, const authority::Scenes &scenes);
    // Entering RUNNING: resets volatile enable flags, latches, counters, and event ring.
    void begin_boot();

    // RUNNING owner-thread volatile toggle (0 WAL). Disabling clears edge latch.
    ayni_status_t set_enabled(ayni_string_v1 rule_id, uint32_t enabled);
    // Owner-thread diagnostic snapshot for one registered rule.
    ayni_status_t info(ayni_string_v1 rule_id, ayni_reflex_info_v1 &out) const;
    // Appends one event into the bounded ring, overwriting the oldest entry when full.
    void push_event(ayni_reflex_event_v1 event);
    // Pops the oldest event from the ring; returns AYNI_NOT_FOUND when empty.
    ayni_status_t take_event(ayni_reflex_event_v1 &out);

    uint32_t count() const { return rule_count_; }
    uint32_t ordered_index(uint32_t pos) const { return order_[pos]; }
    ReflexRuleEntry &entry(uint32_t index) { return rules_[index]; }
    const ReflexRuleEntry &entry(uint32_t index) const { return rules_[index]; }
    ayni_bytes_v1 input(uint32_t index) const;
    // Owner-only scratch borrow through synchronous Call admission; no trigger-time decoding.
    ayni_bytes_v1
    projected_input(uint32_t index,
                    const std::array<int64_t, AYNI_REFLEX_CONDITION_COUNT_MAX> &values);

  private:
    bool find(ayni_string_v1 rule_id, uint32_t &out_index) const;
    uint8_t *input_slot(uint32_t index) const;
    ayni_status_t validate_rule_links(uint32_t index, foundation::Workspace &workspace,
                                      const Catalogs &catalogs, const work::AgentTable &agents,
                                      const authority::Scenes &scenes);
    ayni_status_t normalize_rule_input(uint32_t index, foundation::Workspace &workspace,
                                       const Catalogs &catalogs,
                                       const catalog::BindingLinks &links);
    void sort_priority_order();

    ReflexRuleEntry *rules_;        // Fixed array of rule_capacity_ entries, or null.
    uint8_t *inputs_;               // rule_capacity_ input strides plus one normalization scratch.
    ayni_reflex_event_v1 *events_;  // Fixed ring of event_capacity_ entries, or null.
    const uint32_t rule_capacity_;  // Configured reflex_capacity.
    const uint32_t input_stride_;   // Configured call_input_max_bytes (0 when disabled).
    const uint32_t event_capacity_; // Configured reflex_event_capacity.
    uint32_t rule_count_ = 0;       // Registered rules, at most rule_capacity_.
    std::array<uint32_t, AYNI_REFLEX_CAPACITY_MAX> order_{}; // Descending-priority eval order.
    uint32_t event_head_ = 0;     // Index of oldest retained event in ring.
    uint32_t event_count_ = 0;    // Occupied ring entries, at most event_capacity_.
    uint64_t event_sequence_ = 0; // Monotonic boot-local event serial.
    uint64_t dropped_total_ = 0;  // Total older events overwritten due to ring bound.
};

} // namespace ayni::kernel
#endif
