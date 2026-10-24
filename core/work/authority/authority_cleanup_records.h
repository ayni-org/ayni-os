// Work scan progress layout, contracts/storage/records.md type 19/v1.
#ifndef AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_RECORDS_H
#define AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_RECORDS_H

#include "work/authority/authority_cleanup.h"

#include <cstdint>

namespace ayni::work {

constexpr uint16_t authority_cleanup_record_type = 19;
constexpr uint16_t authority_cleanup_record_version = 1;
constexpr uint32_t authority_cleanup_record_size = 44;

// Caller supplies a shared-validated fact and exactly record_size writable bytes.
void write_authority_cleanup_record(const AuthorityCleanupRecord &record, uint8_t *body);
// Exact bounded parsing and shared shape validation; failure preserves all output.
bool read_authority_cleanup_record(const uint8_t *body, uint32_t size, AuthorityCleanupRecord &out);

} // namespace ayni::work
#endif // AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_RECORDS_H
