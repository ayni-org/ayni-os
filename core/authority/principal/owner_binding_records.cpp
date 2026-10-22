#include "authority/principal/owner_binding_records.h"

#include "authority/principal/principal_registry.h"
#include "durability/batch/record_body.h"

namespace ayni::authority {

void write_owner_binding_record(const OwnerBinding &binding, uint8_t *body) {
    durability::BodyWriter writer(body, owner_binding_record_size);
    writer.u64(binding.principal_id);
    writer.u32(binding.binding_revision);
    writer.u32(0);
}

bool read_owner_binding_record(const uint8_t *body, uint32_t size, OwnerBindingRecord &out) {
    if (body == nullptr || size != owner_binding_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    const OwnerBindingRecord record{reader.u64(), reader.u32()};
    const uint32_t reserved = reader.u32();
    if (!reader.ok() || record.principal_id == 0U || record.binding_revision == 0U ||
        reserved != 0U) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::authority
