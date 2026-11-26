#include "work/execution/execution_fact.h"
#include "foundation/kernel_id.h"
#include <cstring>
namespace ayni::work {
namespace {
bool read_id(foundation::BodyReader &reader, catalog::CatalogId &out) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(catalog::CatalogId::max_size, size);
    return out.assign({reinterpret_cast<const char *>(bytes), size});
}
void write_id(const catalog::CatalogId &id, foundation::BodyWriter &writer) {
    writer.bytes(id.data(), id.size());
}
bool same_epochs(const ayni_permit_epochs_v1 &a, const ayni_permit_epochs_v1 &b) {
    return a.kernel_boot_epoch == b.kernel_boot_epoch && a.authority_epoch == b.authority_epoch &&
           a.executor_boot_epoch == b.executor_boot_epoch && a.safety_epoch == b.safety_epoch &&
           a.calibration_version == b.calibration_version;
}
bool same_reference(const ayni_observation_ref_v1 &a, const ayni_observation_ref_v1 &b) {
    return a.source_id.size == b.source_id.size &&
           std::memcmp(a.source_id.data, b.source_id.data, a.source_id.size) == 0 &&
           a.source_version == b.source_version && a.boot_epoch == b.boot_epoch &&
           a.generation == b.generation && a.sequence == b.sequence &&
           a.subscription_id == b.subscription_id;
}
} // namespace
bool execution_stopped(const ExecutionFact &fact) {
    return fact.phase == AYNI_EXECUTION_STOPPED || fact.phase == AYNI_EXECUTION_NOT_STARTED;
}
bool valid_execution_stop(ayni_execution_phase_t phase, ayni_control_stop_cause_t cause,
                          ayni_execution_target_t target) {
    return (phase == AYNI_EXECUTION_STOPPED || phase == AYNI_EXECUTION_NOT_STARTED) &&
           cause <= AYNI_CONTROL_STOP_CLOCK && target <= AYNI_TARGET_UNSATISFIED &&
           (phase == AYNI_EXECUTION_STOPPED
                ? cause != AYNI_CONTROL_STOP_NONE
                : cause == AYNI_CONTROL_STOP_NONE && target == AYNI_TARGET_UNVERIFIED);
}
bool valid_execution_metadata(const ExecutionFact &fact) {
    const auto &epochs = fact.epochs;
    if (catalog::validate_execution_binding(fact.binding) != AYNI_OK || fact.scene_seq == 0 ||
        fact.control_domain == 0 || fact.control_mode == 0 || epochs.kernel_boot_epoch == 0 ||
        epochs.authority_epoch == 0 || epochs.executor_boot_epoch == 0 ||
        epochs.safety_epoch == 0 || epochs.calibration_version == 0 || epochs.reserved != 0 ||
        fact.phase < AYNI_EXECUTION_PREPARED || fact.phase > AYNI_EXECUTION_NOT_STARTED ||
        (execution_stopped(fact) ? !valid_execution_stop(fact.phase, fact.stop_cause, fact.target)
                                 : fact.stop_cause != AYNI_CONTROL_STOP_NONE ||
                                       fact.target != AYNI_TARGET_UNVERIFIED) ||
        fact.dependency_count > fact.dependencies.size()) {
        return false;
    }
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        const auto &ref = fact.dependencies[at];
        if (ref.source_id.size > AYNI_CATALOG_ID_MAX_BYTES ||
            !catalog::CatalogId::is_valid({ref.source_id.data, ref.source_id.size}) ||
            ref.source_version == 0 || ref.boot_epoch != epochs.kernel_boot_epoch ||
            (ref.subscription_id != 0 &&
             (!foundation::is_kernel_id(ref.subscription_id) ||
              foundation::id_boot_epoch(ref.subscription_id) > ref.boot_epoch)) ||
            ref.generation == 0 || ref.sequence == 0) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            const auto &other = fact.dependencies[before].source_id;
            if (other.size == ref.source_id.size &&
                std::memcmp(other.data, ref.source_id.data, other.size) == 0) {
                return false;
            }
        }
    }
    return true;
}
bool valid_execution(const ExecutionFact &fact, ayni_bytes_v1 arguments) {
    ayni_control_envelope_v1 envelope{};
    return valid_execution_metadata(fact) &&
           catalog::execution_envelope(fact.binding, arguments, envelope) == AYNI_OK;
}
bool same_execution_identity(const ExecutionFact &a, const ExecutionFact &b) {
    if (!catalog::same_execution_binding(a.binding, b.binding) || a.scene_seq != b.scene_seq ||
        a.control_domain != b.control_domain || a.control_mode != b.control_mode ||
        !same_epochs(a.epochs, b.epochs) || a.dependency_count != b.dependency_count) {
        return false;
    }
    for (uint32_t at = 0; at < a.dependency_count; ++at) {
        if (!same_reference(a.dependencies[at], b.dependencies[at])) {
            return false;
        }
    }
    return true;
}
bool execution_advances(const std::optional<ExecutionFact> &before,
                        const std::optional<ExecutionFact> &after) {
    if (before.has_value() != after.has_value()) {
        return false;
    }
    if (!before) {
        return true;
    }
    if (!same_execution_identity(*before, *after)) {
        return false;
    }
    if (execution_stopped(*before)) {
        return before->phase == after->phase && before->target == after->target &&
               before->stop_cause == after->stop_cause;
    }
    return after->phase == before->phase ||
           (before->phase == AYNI_EXECUTION_PREPARED &&
            after->phase == AYNI_EXECUTION_DISPATCHING) ||
           execution_stopped(*after);
}
uint32_t execution_record_max() {
    constexpr uint32_t identity_count = 3;
    constexpr uint32_t field_identity_count = 2;
    return execution_record_fixed_bytes + identity_count * catalog::CatalogId::max_size +
           AYNI_CONTROL_FIELD_COUNT_MAX *
               (execution_field_fixed_bytes + field_identity_count * catalog::CatalogId::max_size) +
           AYNI_OBSERVATION_CONDITION_COUNT_MAX *
               (execution_dependency_fixed_bytes + catalog::CatalogId::max_size);
}
uint32_t execution_record_size(const ExecutionFact &fact) {
    const auto &binding = fact.binding;
    uint32_t size = execution_record_fixed_bytes + binding.binding.size() +
                    binding.executor.size() + binding.scene.size();
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        size += execution_field_fixed_bytes + binding.fields[at].id.size() +
                binding.fields[at].unit.size();
    }
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        size += execution_dependency_fixed_bytes + fact.dependencies[at].source_id.size;
    }
    return size;
}
// Stable declaration first, then local generation and stop facts; parameter values
// remain in Call's one canonical input, avoiding a second authorization projection.
void write_execution(const ExecutionFact &fact, foundation::BodyWriter &writer) {
    const auto &binding = fact.binding;
    write_id(binding.binding, writer);
    writer.u32(binding.binding_version);
    write_id(binding.executor, writer);
    writer.u32(binding.executor_version);
    write_id(binding.scene, writer);
    writer.u32(binding.scene_version);
    writer.u32(binding.form);
    writer.u32(binding.lease_us);
    writer.u32(binding.max_command_age_us);
    writer.u32(binding.field_count);
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        const auto &field = binding.fields[at];
        write_id(field.id, writer);
        write_id(field.unit, writer);
        writer.u32(field.minimum_field);
        writer.u32(field.maximum_field);
    }
    writer.u64(fact.scene_seq);
    writer.u64(fact.control_domain);
    writer.u32(fact.control_mode);
    writer.u64(fact.epochs.kernel_boot_epoch);
    writer.u64(fact.epochs.authority_epoch);
    writer.u64(fact.epochs.executor_boot_epoch);
    writer.u64(fact.epochs.safety_epoch);
    writer.u32(fact.epochs.calibration_version);
    writer.u32(fact.phase);
    writer.u32(fact.target);
    writer.u32(fact.stop_cause);
    writer.u32(fact.dependency_count);
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        const auto &ref = fact.dependencies[at];
        writer.bytes(ref.source_id.data, ref.source_id.size);
        writer.u32(ref.source_version);
        writer.u64(ref.boot_epoch);
        writer.u64(ref.generation);
        writer.u64(ref.sequence);
        writer.u64(ref.subscription_id);
    }
}
bool read_execution(foundation::BodyReader &reader, ExecutionFact &out) {
    ExecutionFact fact{};
    auto &binding = fact.binding;
    if (!read_id(reader, binding.binding)) {
        return false;
    }
    binding.binding_version = reader.u32();
    if (!read_id(reader, binding.executor)) {
        return false;
    }
    binding.executor_version = reader.u32();
    if (!read_id(reader, binding.scene)) {
        return false;
    }
    binding.scene_version = reader.u32();
    binding.form = reader.u32();
    binding.lease_us = reader.u32();
    binding.max_command_age_us = reader.u32();
    binding.field_count = reader.u32();
    if (binding.field_count > binding.fields.size()) {
        return false;
    }
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        auto &field = binding.fields[at];
        if (!read_id(reader, field.id) || !read_id(reader, field.unit)) {
            return false;
        }
        field.minimum_field = reader.u32();
        field.maximum_field = reader.u32();
    }
    fact.scene_seq = reader.u64();
    fact.control_domain = reader.u64();
    fact.control_mode = reader.u32();
    fact.epochs.kernel_boot_epoch = reader.u64();
    fact.epochs.authority_epoch = reader.u64();
    fact.epochs.executor_boot_epoch = reader.u64();
    fact.epochs.safety_epoch = reader.u64();
    fact.epochs.calibration_version = reader.u32();
    fact.phase = reader.u32();
    fact.target = reader.u32();
    fact.stop_cause = reader.u32();
    fact.dependency_count = reader.u32();
    if (fact.dependency_count > fact.dependencies.size()) {
        return false;
    }
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        auto &ref = fact.dependencies[at];
        catalog::CatalogId source;
        if (!read_id(reader, source)) {
            return false;
        }
        ref.source_id = source.snapshot();
        ref.source_version = reader.u32();
        ref.boot_epoch = reader.u64();
        ref.generation = reader.u64();
        ref.sequence = reader.u64();
        ref.subscription_id = reader.u64();
    }
    out = fact;
    return true;
}
} // namespace ayni::work
