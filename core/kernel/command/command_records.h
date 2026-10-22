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
constexpr uint32_t command_record_size = 52; // ordinary v1
constexpr uint16_t exact_command_record_version = 2;
constexpr uint32_t exact_command_record_fixed_size = command_record_size + 8U;
constexpr uint16_t revoke_command_record_version = 3;
constexpr uint32_t revoke_command_record_fixed_size = command_record_size + 12U;
constexpr uint32_t command_record_max(uint32_t canonical_max_bytes) {
    const uint32_t create_max = canonical_max_bytes == 0U
                                    ? command_record_size
                                    : exact_command_record_fixed_size + canonical_max_bytes;
    constexpr uint32_t revoke_max = revoke_command_record_fixed_size + revoke_command_size;
    return canonical_max_bytes == 0U || create_max >= revoke_max ? create_max : revoke_max;
}
uint16_t command_record_version_for(const CommandMapping &mapping);
uint32_t command_record_size_for(const CommandMapping &mapping);

// v2 create/v3 revoke require complete exact identity, matching digest and effect;
// failure changes no output. size is exactly command_record_size_for().
bool write_command_record(const CommandMapping &mapping, uint64_t floor, ayni_bytes_v1 canonical,
                          uint8_t *body, uint32_t size);
// Record version determines syntax. Views borrow body. Failure changes no
// outputs; restored commit_seq remains 0 until the envelope supplies it.
bool read_command_record(uint16_t version, const uint8_t *body, uint32_t size,
                         CommandMapping &out_mapping, uint64_t &out_floor,
                         ayni_bytes_v1 &out_canonical);

// body has command_record_size bytes.
void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body);
// Returns false for a body of another size; out_mapping.commit_seq is left 0.
bool read_command_record(const uint8_t *body, uint32_t size, CommandMapping &out_mapping,
                         uint64_t &out_floor);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_RECORDS_H
