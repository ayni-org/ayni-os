// Adapter representations are resolved here, using catalog-owned Schema semantics.
// No JSON DOM, vendor SDK, live directory or authorization enters this module.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_ARGUMENT_MAPPING_H
#define AYNI_CORE_CATALOG_CAPABILITY_ARGUMENT_MAPPING_H
#include "catalog/capability/value_validation.h"
#include <ayni/arguments.h>

namespace ayni::catalog {
// Schema-independent shape and exact numeric-lexeme admission.
ayni_status_t validate_arguments(const ayni_arguments_v1 &input);
// Admitted static Schema; maps names and lexemes to stable IDs/precise kinds.
// Output text borrows input. Defaults and canonical order remain the common
// normalizer's responsibility; failures preserve the complete previous output.
ayni_status_t map_arguments(const ayni_schema_v1 &schema, const ayni_arguments_v1 &input,
                            DecodedValue &out);
// The decoder admitted this unchanged source tree in this same owner boundary;
// resolve names/types without a second source-shape/lexeme admission pass.
ayni_status_t map_admitted_arguments(const ayni_schema_v1 &schema, const ayni_arguments_v1 &input,
                                     DecodedValue &out);
} // namespace ayni::catalog
#endif
