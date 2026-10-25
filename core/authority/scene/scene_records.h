// REQUIRED type 22/v1 Scene state, contracts/storage/records.md; same body in
// journal and snapshot. Original definition is independent of the startup catalog.
#ifndef AYNI_CORE_AUTHORITY_SCENE_RECORDS_H
#define AYNI_CORE_AUTHORITY_SCENE_RECORDS_H
#include "authority/scene/scene_table.h"
namespace ayni::authority {
constexpr uint16_t scene_record_type = 22;
constexpr uint16_t scene_record_version = 1;
constexpr uint32_t scene_record_fixed_bytes = 44;
// Read result borrows definition bytes from its containing record.
struct SceneRecord {
    SceneState state;         // Valid identity, revision, mode and stamp.
    ayni_bytes_v1 definition; // Complete original canonical declaration.
};
uint32_t scene_record_size(const SceneState &state); // Exact body bytes.
uint32_t scene_record_max(uint32_t scope_bytes);     // Worst-case configured body.
void write_scene_record(const SceneState &state, ayni_bytes_v1 definition, uint8_t *body);
bool read_scene_record(const uint8_t *body, uint32_t size, SceneRecord &out);
} // namespace ayni::authority
#endif
