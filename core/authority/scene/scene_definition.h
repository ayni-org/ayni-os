// Canonical Scene declaration codec, shared by startup registration and durable facts.
#ifndef AYNI_CORE_AUTHORITY_SCENE_DEFINITION_H
#define AYNI_CORE_AUTHORITY_SCENE_DEFINITION_H
#include "authority/scene/scene_table.h"
namespace ayni::authority {
// records.md Scene definition: two counts; rules contain ID/version/domain/Scope;
// conditions contain original source types and bounded range/age predicates.
constexpr uint32_t scene_definition_header_bytes = 8;
constexpr uint32_t scene_rule_fixed_bytes = 16;
constexpr uint32_t scene_condition_fixed_bytes = 52;
// One scratch canonical Scope stride per rule plus the completed declaration.
uint32_t scene_definition_scratch_bytes(uint32_t scope_bytes);
// Validate/normalize once, then encode into scratch. Output metadata points into
// the returned canonical bytes; all caller descriptor memory is released on return.
ayni_status_t build_scene_definition(const ayni_scene_desc_v1 &input,
                                     const catalog::CapabilityRegistry &capabilities,
                                     const catalog::ObservationSources &sources,
                                     uint32_t scope_bytes, uint8_t *scratch, SceneDefinition &out,
                                     ayni_bytes_v1 &bytes);
// Checks an original declaration without consulting this boot's changed catalogs.
// Failure preserves output; metadata borrows no input pointers.
bool read_scene_definition(ayni_bytes_v1 bytes, SceneDefinition &out);
} // namespace ayni::authority
#endif
