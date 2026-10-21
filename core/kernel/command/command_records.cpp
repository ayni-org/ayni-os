#include "kernel/command/command_records.h"

#include "durability/batch/record_body.h"

namespace ayni::kernel {

void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body) {
    durability::BodyWriter writer(body, command_record_size);
    writer.u64(mapping.principal_id);
    writer.u64(mapping.command_id);
    writer.u64(mapping.digest);
    writer.u64(floor);
    writer.u32(mapping.kind);
    writer.u32(mapping.object.kind);
    writer.u64(mapping.object.id);
    writer.u32(mapping.object.revision);
}

bool read_command_record(const uint8_t *body, uint32_t size, CommandMapping &out_mapping,
                         uint64_t &out_floor) {
    durability::BodyReader reader(body, size);
    out_mapping = CommandMapping{};
    out_mapping.principal_id = reader.u64();
    out_mapping.command_id = reader.u64();
    out_mapping.digest = reader.u64();
    out_floor = reader.u64();
    out_mapping.kind = reader.u32();
    out_mapping.object.kind = reader.u32();
    out_mapping.object.id = reader.u64();
    out_mapping.object.revision = reader.u32();
    return reader.ok();
}

} // namespace ayni::kernel
