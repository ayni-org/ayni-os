// Registered goal type versions (C-POLICY-009). A goal type is known by its
// catalog id and version; several versions of one id coexist, so a goal
// keeps the semantics of the version it was created with. Registration is
// closed once the kernel leaves CONFIGURING (C-LIFE-006), so an index stays
// valid for the kernel's life. A goal type names the strategy that decides
// its goals' tasks by id; ayni_start resolves the ids (C-LIFE-003), so the
// order of registration does not matter.

#ifndef AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H
#define AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H

#include "catalog/catalog_id.h"
#include "catalog/goal_type/work_profile.h"
#include "catalog/strategy/strategy_registry.h"
#include "foundation/bounded_array.h"

#include <ayni/ayni.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

// One tool a goal type's model calls may be offered (ADR-0054): an exact
// Binding, which grants no authority by being listed.
struct GoalTypeTool {
    static constexpr uint32_t no_binding = 0xFFFFFFFFU;

    CatalogId binding_id;
    uint32_t binding_version = 0;
    // In the binding registry, once link_tools() succeeded.
    uint32_t binding_index = no_binding;
};

enum class GoalContext : uint32_t { none = 0, agent = 1 }; // Trusted input provenance declaration.
struct GoalType {
    static constexpr uint32_t no_strategy = 0xFFFFFFFFU;

    CatalogId id;
    uint32_t version = 0;
    void *ctx = nullptr;
    ayni_goal_validate_fn validate = nullptr; // null: the type takes no parameters
    uint32_t trigger_kinds = 0;               // AYNI_TRIGGER_BIT of each kind it accepts
    // How long a due occurrence may wait for a task (ADR-0020);
    // AYNI_OCCURRENCE_WAIT_UNBOUNDED: no limit.
    uint64_t occurrence_wait_max_us = 0;
    // Empty exactly when trigger_kinds holds NONE alone: such goals have no
    // tasks to decide (ADR-0022).
    CatalogId strategy_id;
    // In the strategy registry, once link_strategies() succeeded; no_strategy
    // for an empty strategy_id.
    uint32_t strategy_index = no_strategy;
    // Trusted declaration callback; null means explicitly no new-Goal needs.
    ayni_goal_requirements_fn export_requirements = nullptr;
    WorkProfile work_profile{}; // Immutable trusted scheduling semantics, copied into Goals.
    // The allow list in declared order; tools[0, tool_count) are distinct by binding id.
    std::array<GoalTypeTool, AYNI_GOAL_TYPE_TOOL_COUNT_MAX> tools{};
    uint32_t tool_count = 0;
    GoalContext context_mode = GoalContext::none; // Agent mode captures its current file sources.
};

class GoalTypeRegistry {
  public:
    // The trigger kinds this library defines (ADR-0018, ADR-0019,
    // ADR-0021), as trigger_kinds bits; work/goal schedules or records their
    // occurrences.
    static constexpr uint32_t defined_trigger_kinds =
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_DELAY) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_LOCAL_INPUT) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_ONCE) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_DAILY) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_WEEKLY);
    static bool defines_trigger(ayni_trigger_kind_t kind) {
        return kind < 32U && (defined_trigger_kinds & AYNI_TRIGGER_BIT(kind)) != 0U;
    }

    enum class AddResult { added, duplicate, full };

    struct Lookup {
        enum class Kind { found, unknown_id, unknown_version } kind;
        uint32_t index; // when found
    };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<GoalType>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<GoalType>::storage_size(capacity);
    }

    // storage as for BoundedArray<GoalType>.
    GoalTypeRegistry(void *storage, uint32_t capacity) : types_(storage, capacity) {}

    // type has a valid id, a nonzero version, trigger_kinds that is a
    // nonempty subset of defined_trigger_kinds and a strategy_id that is
    // empty exactly when trigger_kinds holds NONE alone.
    AddResult add(const GoalType &type);

    Lookup find(ayni_string_v1 id, uint32_t version) const;
    // Any current trusted version declares this ID as context-bearing. Used only when the
    // saved exact version is unavailable, so unknown ordinary work keeps its legacy semantics.
    bool has_agent_context(ayni_string_v1 id) const;

    // index from a found Lookup.
    const GoalType &at(uint32_t index) const { return types_[index]; }

    // Whether the type at index accepts these parameters: none when it has
    // no validator, otherwise whatever the validator returns AYNI_OK for.
    bool accepts(uint32_t index, ayni_bytes_v1 parameters) const;
    // Whether goals of the type at index may choose trigger.
    bool accepts_trigger(uint32_t index, ayni_trigger_kind_t trigger) const;

    // Resolves the strategy id of every type. Returns false when one names
    // a strategy that is not registered; the indexes are then incomplete,
    // and ayni_start, which stays in CONFIGURING, resolves them again on
    // its next call.
    bool link_strategies(const StrategyRegistry &strategies);
    // Resolves every tool of every type: resolve(tool, out_index) returns
    // AYNI_OK with the Binding's index, or why it cannot be a tool. Returns
    // the first failure, leaving the indexes incomplete for ayni_start to
    // resolve again, as link_strategies does.
    template <typename Resolve> ayni_status_t link_tools(Resolve resolve) {
        for (uint32_t index = 0; index < types_.size(); ++index) {
            GoalType &type = types_[index];
            for (uint32_t at = 0; at < type.tool_count; ++at) {
                const ayni_status_t status = resolve(type.tools[at], type.tools[at].binding_index);
                if (status != AYNI_OK) {
                    return status;
                }
            }
        }
        return AYNI_OK;
    }

  private:
    foundation::BoundedArray<GoalType> types_;
};

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H
