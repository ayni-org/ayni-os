#include "catalog/capability/workspace.h"
#include "catalog/capability/business_encoding.h"

namespace ayni::catalog {
namespace {
// read_normalized_value holds two candidates while its normalizer holds another
// transient tree. Alignment slack covers all lexical leases on this path.
constexpr std::size_t value_candidates = 3;
constexpr std::size_t lease_alignment_slots = 32;
// An owner proof, decode candidate and enclosing input/result proof can overlap
// during restored-call admission; schema/default scratch is reused sequentially.
constexpr std::size_t business_candidates = 3;
constexpr std::size_t schema_buffers = 4;
constexpr std::size_t result_candidates = 2;
} // namespace

std::size_t value_workspace_size() {
    return normalization_workspace_size() + value_candidates * sizeof(DecodedValue) +
           lease_alignment_slots * foundation::Workspace::alignment;
}

std::size_t business_workspace_size() {
    return business_candidates * sizeof(DecodedBusiness) + sizeof(DecodedSchema) +
           business_encoding_bytes_max + schema_buffers * schema_encoding_bytes_max +
           result_candidates * sizeof(DecodedValue) + value_workspace_size();
}
} // namespace ayni::catalog

extern "C" ayni_status_t ayni_value_workspace_requirements(size_t *bytes, size_t *alignment) {
    if (bytes == nullptr || alignment == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *bytes = ayni::catalog::value_workspace_size();
    *alignment = ayni::foundation::Workspace::alignment;
    return AYNI_OK;
}

extern "C" ayni_status_t ayni_business_workspace_requirements(size_t *bytes, size_t *alignment) {
    if (bytes == nullptr || alignment == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *bytes = ayni::catalog::business_workspace_size();
    *alignment = ayni::foundation::Workspace::alignment;
    return AYNI_OK;
}
