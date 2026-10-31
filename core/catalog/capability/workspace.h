// Synchronous codec working-memory bounds. Catalog owns these representations;
// Kernel and external callers own actual memory and its platform placement.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_WORKSPACE_H
#define AYNI_CORE_CATALOG_CAPABILITY_WORKSPACE_H
#include "foundation/workspace.h"

namespace ayni::catalog {
// The normalizer owns its private traversal representation and reports its size.
std::size_t normalization_workspace_size();
// Complete nested native-value operations, including transaction candidates.
std::size_t value_workspace_size();
// Complete original-proof decode/validation, including enclosing owner candidates.
std::size_t business_workspace_size();
} // namespace ayni::catalog
#endif
