#include "catalog/goal_type/goal_type_registry.h"

namespace ayni::catalog {
namespace {

ayni_string_v1 view(const CatalogId &id) {
    return {id.data(), id.size()};
}

} // namespace

GoalTypeRegistry::AddResult GoalTypeRegistry::add(const GoalType &type) {
    const Lookup existing = find(view(type.id), type.version);
    if (existing.kind == Lookup::Kind::found) {
        return AddResult::duplicate;
    }
    return types_.push_back(type) ? AddResult::added : AddResult::full;
}

GoalTypeRegistry::Lookup GoalTypeRegistry::find(ayni_string_v1 id, uint32_t version) const {
    Lookup lookup{Lookup::Kind::unknown_id, 0};
    for (uint32_t index = 0; index < types_.size(); ++index) {
        const GoalType &type = types_[index];
        if (!type.id.equals(id)) {
            continue;
        }
        if (type.version == version) {
            return {Lookup::Kind::found, index};
        }
        lookup.kind = Lookup::Kind::unknown_version;
    }
    return lookup;
}

bool GoalTypeRegistry::accepts(uint32_t index, ayni_bytes_v1 parameters) const {
    const GoalType &type = types_[index];
    if (type.validate == nullptr) {
        return parameters.size == 0U;
    }
    return type.validate(type.ctx, parameters) == AYNI_OK;
}

bool GoalTypeRegistry::accepts_trigger(uint32_t index, ayni_trigger_kind_t trigger) const {
    return trigger < 32U && (types_[index].trigger_kinds & AYNI_TRIGGER_BIT(trigger)) != 0U;
}

bool GoalTypeRegistry::link_strategies(const StrategyRegistry &strategies) {
    for (uint32_t index = 0; index < types_.size(); ++index) {
        GoalType &type = types_[index];
        if (type.strategy_id.size() == 0U) {
            type.strategy_index = GoalType::no_strategy;
        } else if (!strategies.find(type.strategy_id, type.strategy_index)) {
            return false;
        }
    }
    return true;
}

} // namespace ayni::catalog
