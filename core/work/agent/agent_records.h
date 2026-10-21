// The INSTANCE record (contracts/storage/records.md, ADR-0026): an agent
// instance as its last change left it, in the batch of every transition
// that creates or changes it. Its stamp is that batch's.

#ifndef AYNI_CORE_WORK_AGENT_AGENT_RECORDS_H
#define AYNI_CORE_WORK_AGENT_AGENT_RECORDS_H

#include "work/agent/agent_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

constexpr uint16_t instance_record_type = 2;
constexpr uint16_t instance_record_version = 1;
constexpr uint32_t instance_record_size = 24;

struct InstanceRecord {
    uint64_t agent_id;
    uint64_t instance_id;
    uint32_t revision;
    ayni_agent_lifecycle_t lifecycle;
};

// body has instance_record_size bytes.
void write_instance_record(const AgentInstance &instance, uint8_t *body);
// Returns false for a body of another size.
bool read_instance_record(const uint8_t *body, uint32_t size, InstanceRecord &out);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_AGENT_AGENT_RECORDS_H
