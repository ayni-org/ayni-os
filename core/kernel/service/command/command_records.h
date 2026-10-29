// The COMMAND_MAPPING record (contracts/storage/records.md, ADR-0026): a
// command the transition accepted, its result and the principal's floor
// after it, in the batch of that transition. Its commit_seq is the batch's.

#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H

#include "durability/batch/record_types.h"
#include "kernel/service/command/command_table.h"
#include "knowledge/memory/memory_records.h"

#include <cstdint>

namespace ayni::kernel {

constexpr uint16_t command_record_type = durability::record_type::command_mapping;
constexpr uint16_t command_record_version = 1;
constexpr uint16_t command_snapshot_record_version = 2; // Adds original acceptance sequence.
// Identity 52 B plus content/effect/format and two byte-string lengths, records.md schema 3.
constexpr uint32_t command_record_fixed_size = 72;
constexpr uint32_t command_record_size = command_record_fixed_size + sizeof(uint32_t);
// Source framing is always present, including its explicit zero-count representation.
bool valid_command_sources(const CommandMapping &mapping);
constexpr uint32_t command_record_max(uint32_t canonical_max_bytes, uint32_t dependency_max = 0) {
    const auto request = canonical_max_bytes > revoke_command_size
                             ? canonical_max_bytes : revoke_command_size;
    return command_record_fixed_size + request + knowledge::memory_dependencies_size(dependency_max);
}
uint32_t command_record_size_for(const CommandMapping &mapping);

// Exact commands require complete identity, matching digest and effect;
// failure changes no output. size is exactly command_record_size_for().
bool write_command_record(const CommandMapping &mapping, uint64_t floor, ayni_bytes_v1 canonical,
                          uint8_t *body, uint32_t size, ayni_bytes_v1 result = {});
// Only the complete current body is read. Views borrow body. Failure changes no
// outputs; restored commit_seq remains 0 until the envelope supplies it.
bool read_command_record(const uint8_t *body, uint32_t size,
                         CommandMapping &out_mapping, uint64_t &out_floor,
                         ayni_bytes_v1 &out_canonical, ayni_bytes_v1 *out_result = nullptr);

// body has command_record_size bytes.
void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body);
// Returns false for a body of another size; out_mapping.commit_seq is left 0.
bool read_command_record(const uint8_t *body, uint32_t size, CommandMapping &out_mapping,
                         uint64_t &out_floor);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H
