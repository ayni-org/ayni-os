// The COMMAND_MAPPING record (contracts/storage/records.md, ADR-0026): a
// command the transition accepted, its result and the principal's floor
// after it, in the batch of that transition. Its commit_seq is the batch's.

#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H

#include "kernel/command/command_table.h"

#include <cstdint>

namespace ayni::kernel {

constexpr uint16_t command_record_type = 9;
constexpr uint16_t command_record_version = 1;
constexpr uint32_t command_record_size = 52;

// body has command_record_size bytes.
void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body);
// Returns false for a body of another size; out_mapping.commit_seq is left 0.
bool read_command_record(const uint8_t *body, uint32_t size, CommandMapping &out_mapping,
                         uint64_t &out_floor);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H
