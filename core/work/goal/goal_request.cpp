#include "work/goal/goal_request.h"
#include "catalog/catalog_id.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "work/goal/calendar_table.h"
#include "work/goal/pause_groups.h"
namespace ayni::work {
namespace {
// A local input trigger names its input by a catalog id; other kinds take
// none.
bool trigger_source_fits(const GoalCreate &create) {
    return create.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT
               ? catalog::CatalogId::is_valid(create.trigger_source)
               : create.trigger_source.size == 0U;
}

} // namespace

ayni_status_t check_goal_form(const GoalCreate &create) {
    return !catalog::CatalogId::is_valid(create.goal_type_id) ||
                   (create.parameters.data == nullptr && create.parameters.size != 0U) ||
                   !catalog::GoalTypeRegistry::defines_trigger(create.trigger.kind) ||
                   !create.trigger.fits() || !PauseGroups::valid_membership(create.pause_group) ||
                   !trigger_source_fits(create) ||
                   create.calendar.kind != calendar_kind(create.trigger.kind)
               ? AYNI_INVALID_ARGUMENT
               : AYNI_OK;
}

} // namespace ayni::work
