// Memory access and export are separate Grant checks; actual record, purpose and destination bind
// them.
#ifndef AYNI_CORE_KERNEL_MEMORY_PERMISSIONS_H
#define AYNI_CORE_KERNEL_MEMORY_PERMISSIONS_H
#include "catalog/binding/binding_registry.h"
#include "kernel/service/memory/memory_access.h"
namespace ayni::kernel {
// Stable M1 capability schema fields, documented in contracts/domain/memory.md.
constexpr uint32_t memory_capability_version = 1;
constexpr uint32_t memory_epoch_field = 1;
constexpr uint32_t memory_sequence_field = 2;
constexpr uint32_t memory_purpose_field = 3;
constexpr uint32_t memory_operation_field = 4;
constexpr uint32_t memory_destination_field = 4;
constexpr uint32_t memory_scope_fields = 4;
constexpr int64_t memory_read_operation = 1;
constexpr int64_t memory_write_operation = 2;
// Canonical typed operands, reused by public management scope builders and every permission check.
std::array<ayni_scope_field_v1, memory_scope_fields>
memory_read_fields(uint64_t id, knowledge::MemoryPurpose purpose);
std::array<ayni_scope_field_v1, memory_scope_fields>
memory_export_fields(uint64_t id, knowledge::MemoryPurpose purpose,
                     const ayni_string_v1 *destination);
// Immutable captured identity/scope proof, independent of later expiry or revocation.
// History may contain unavailable sources; consumers additionally check current validity.
bool memory_dependencies_history(const WorkModel &model,
                                 const knowledge::MemoryDependencies &dependencies,
                                 uint64_t boot_epoch_limit, uint64_t actor = 0);
// Static product boundary: organization cannot read another Agent's private namespace.
bool memory_purpose_allowed(knowledge::MemoryNamespace space, uint64_t namespace_id, uint64_t actor,
                            knowledge::MemoryPurpose purpose);
// Select once at read; private ownership needs no sharing Grant. No body is exposed on refusal.
ayni_status_t memory_read_permission(const MemoryAccess &access, uint64_t actor,
                                     uint32_t record_index, knowledge::MemoryPurpose purpose,
                                     knowledge::MemoryGrantRef &out);
// Recheck persisted work against its actual Binding without selecting new permissions.
bool task_export_current(const MemoryAccess &access, uint32_t task_index,
                         const catalog::Binding &binding);
// Add export permission for this exact destination. Captured permissions never get replaced;
// a dependency exported again must still fit the captured Grant's original immutable scope.
ayni_status_t memory_export_permissions(const MemoryAccess &access, uint64_t actor,
                                        const catalog::Binding &binding,
                                        knowledge::MemoryDependencies &dependencies);
} // namespace ayni::kernel
#endif
