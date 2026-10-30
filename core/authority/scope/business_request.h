// One complete business request is compared with one complete immutable Scope.
#ifndef AYNI_CORE_AUTHORITY_SCOPE_BUSINESS_REQUEST_H
#define AYNI_CORE_AUTHORITY_SCOPE_BUSINESS_REQUEST_H
#include "authority/scope/scope.h"
#include "catalog/binding/binding_registry.h"
#include <ayni/capability_values.h>
namespace ayni::authority {
// Route membership only; ordinary parameter fields impose no route restriction.
bool binding_route_allowed(const catalog::Binding &binding, ayni_bytes_v1 scope);
// Original Schema/value are admitted together before this boundary. No row may
// select another Scope, and all target/destination operands use this Binding.
Coverage contains_business_request(const catalog::Binding &binding, ayni_bytes_v1 domain,
                                   const ayni_business_contract_v1 &business,
                                   const ayni_value_v1 &value, ayni_bytes_v1 scope);
} // namespace ayni::authority
#endif
