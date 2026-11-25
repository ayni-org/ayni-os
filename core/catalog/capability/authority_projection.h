// Static relationship between business values and finite authority fields.
// Catalog owns the path/type/unit proof; authority owns actual Grant coverage.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_AUTHORITY_PROJECTION_H
#define AYNI_CORE_CATALOG_CAPABILITY_AUTHORITY_PROJECTION_H
#include "catalog/capability/field_domain.h"
#include "foundation/workspace.h"
#include <ayni/capability_values.h>
#include <optional>

namespace ayni::catalog {
// A checked path, retaining the complete original declaration (not a Grant).
struct ProjectionShape {
    std::optional<uint32_t> list_node; // Schema node of the one list anchor; none for scalar paths.
};
// Complete static definition independent of registration order. Finite-domain
// cross references are checked when the catalog is closed, before recovery/I/O.
ayni_status_t validate_business_definition(ayni::foundation::Workspace &workspace,
                                           const ayni_business_contract_v1 &business,
                                           ProjectionShape &out);
// Domainless retained proof whose schemas were already decoded and validated.
// Checks every mapping path/type/unit; original domain coverage is not available here.
ayni_status_t validate_business_schemas_admitted(const ayni_business_contract_v1 &business,
                                                 ProjectionShape &out);

// Admitted business schema and complete authority domain; errors preserve out.
// Rejects optional/null paths, floats, multi-axis lists and implicit unit/scale changes.
ayni_status_t validate_projection(ayni::foundation::Workspace &workspace, ayni_bytes_v1 domain,
                                  const ayni_business_contract_v1 &business, ProjectionShape &out);
// Schema decoders already admitted both unchanged schemas at this boundary.
ayni_status_t validate_projection_schemas_admitted(ayni_bytes_v1 domain,
                                                   const ayni_business_contract_v1 &business,
                                                   ProjectionShape &out);
// One row of an admitted, normalized request. All fields share the same list
// index; the caller fixes one requirement/Grant for every row before allowing it.
struct ProjectionRow {
    ProjectionRow() = default;
    // Copies rebase singleton token pointers into the destination's storage.
    ProjectionRow(const ProjectionRow &source);
    ProjectionRow &operator=(const ProjectionRow &source);
    std::array<ayni_scope_field_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
    std::array<ayni_string_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> tokens{};
    uint32_t count = 0; // Complete actual fields, not an authorization decision.
    ayni_scope_v1 view() const {
        return {sizeof(ayni_scope_v1), count, count == 0U ? nullptr : fields.data()};
    }
};
// Validated static mapping plus normalized tree; copied numeric values and
// borrowed token text survive until the request changes. Failure preserves out.
ayni_status_t project_row(const ayni_business_contract_v1 &business, const ayni_value_v1 &value,
                          uint32_t row, ProjectionRow &out);
// Counts the one list anchor from admitted mapping/value, or one for scalar input.
uint32_t projection_rows(const ayni_business_contract_v1 &business, const ayni_value_v1 &value);
} // namespace ayni::catalog
#endif
