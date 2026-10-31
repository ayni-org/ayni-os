// Trusted local executor ports and exact physical binding constraints; no live permits.
#ifndef AYNI_CORE_CATALOG_EXECUTION_REGISTRY_H
#define AYNI_CORE_CATALOG_EXECUTION_REGISTRY_H
#include "catalog/binding/binding_registry.h"
#include "foundation/workspace.h"
#include <ayni/execution.h>
namespace ayni::catalog {
// Numeric projection is read from the same canonical Scope used by authorization.
struct ExecutionField {
    CatalogId id;           // Nonempty local channel name, unique in a binding.
    CatalogId unit;         // Exact common unit of both source fields.
    uint32_t minimum_field; // Positive authority domain field ID.
    uint32_t maximum_field; // Positive ID; may equal minimum_field for a fixed value.
};
struct ExecutionBinding {
    CatalogId binding;         // Exact existing call Binding name.
    uint32_t binding_version;  // Positive exact selection version.
    CatalogId executor;        // Exact trusted local executor name.
    uint32_t executor_version; // Positive implementation contract version.
    CatalogId scene;        // Required finite Scene, resolved by authority owner outside catalog.
    uint32_t scene_version; // Positive exact definition version.
    ayni_execution_form_t form;  // Bounded PROCEDURE or renewable SESSION.
    uint32_t lease_us;           // Positive declared duration, not a timestamp.
    uint32_t max_command_age_us; // Positive local capture-age bound.
    uint32_t field_count;        // Valid prefix, 1..CONTROL_FIELD_COUNT_MAX.
    std::array<ExecutionField, AYNI_CONTROL_FIELD_COUNT_MAX> fields; // Copied unique channels.
};
struct Executor {
    CatalogId id;           // Sole copied name; ports.executor_id is cleared.
    ayni_executor_v1 ports; // Copied scalar limits/callbacks; ctx alone remains runtime-owned.
};
// Typed validation shared with saved execution facts, independent of current catalog.
ayni_status_t validate_execution_binding(const ExecutionBinding &binding);
bool same_execution_binding(const ExecutionBinding &left, const ExecutionBinding &right);
// Actual Scope has already passed authority normalization. Reject missing/non-numeric
// sources and unit/range mismatches; never clamp, substitute or infer a parameter.
ayni_status_t execution_envelope(const ExecutionBinding &binding, ayni_bytes_v1 arguments,
                                 ayni_control_envelope_v1 &out);

class ExecutionRegistry {
  public:
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t executors, uint32_t bindings);
    // Caller owns aligned arena and all callback contexts until successful Kernel destruction.
    ExecutionRegistry(void *storage, uint32_t executors, uint32_t bindings);
    ayni_status_t add_executor(const ayni_executor_v1 &input); // Atomic copied registration.
    ayni_status_t
    add_binding(const ayni_execution_binding_v1 &input); // Atomic copied registration.
    bool find_executor(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    bool find_binding(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    const Executor &executor(uint32_t index) const {
        return executors_[index];
    } // Registered index only.
    const ExecutionBinding &binding(uint32_t index) const {
        return bindings_[index];
    } // Registered index only.
    uint32_t binding_count() const { return bindings_.size(); } // Configured occupied prefix.
    // Startup consistency; physical actions must own an exclusive actuator and one
    // declared executor, while nonphysical bindings cannot obtain an execution permit.
    ayni_status_t validate_all(ayni::foundation::Workspace &workspace,
                               const BindingRegistry &bindings,
                               const CapabilityRegistry &capabilities,
                               const CallCapabilityRegistry &calls,
                               const ResourceRegistry &resources) const;

  private:
    // One executor has one mandatory exclusive actuator shared by all its bindings.
    ayni_status_t validate_ownership(const BindingRegistry &bindings,
                                     const CallCapabilityRegistry &calls,
                                     const ResourceRegistry &resources) const;
    foundation::BoundedArray<Executor> executors_; // Sole immutable trusted port/limit owner.
    foundation::BoundedArray<ExecutionBinding> bindings_; // Sole copied execution mapping owner.
};
} // namespace ayni::catalog
#endif
