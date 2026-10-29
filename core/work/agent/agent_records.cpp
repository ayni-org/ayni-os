#include "work/agent/agent_records.h"

#include "foundation/record_body.h"

namespace ayni::work {

void write_instance_record(const AgentInstance &instance, uint8_t *body) {
    foundation::BodyWriter writer(body, instance_record_size);
    writer.u64(instance.agent_id);
    writer.u64(instance.instance_id);
    writer.u32(instance.revision);
    writer.u32(instance.lifecycle);
}

bool read_instance_record(const uint8_t *body, uint32_t size, InstanceRecord &out) {
    foundation::BodyReader reader(body, size);
    out.agent_id = reader.u64();
    out.instance_id = reader.u64();
    out.revision = reader.u32();
    out.lifecycle = reader.u32();
    return reader.ok();
}

} // namespace ayni::work
