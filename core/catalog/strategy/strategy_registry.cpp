#include "catalog/strategy/strategy_registry.h"

namespace ayni::catalog {

StrategyRegistry::AddResult StrategyRegistry::add(const Strategy &strategy) {
    uint32_t existing = 0;
    if (find(strategy.id, existing)) {
        return AddResult::duplicate;
    }
    return strategies_.push_back(strategy) ? AddResult::added : AddResult::full;
}

bool StrategyRegistry::find(const CatalogId &id, uint32_t &out_index) const {
    for (uint32_t index = 0; index < strategies_.size(); ++index) {
        if (strategies_[index].id.equals({id.data(), id.size()})) {
            out_index = index;
            return true;
        }
    }
    return false;
}

} // namespace ayni::catalog
