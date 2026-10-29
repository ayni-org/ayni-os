// Complete immutable business/execution proof (ADR-0084). Grant, Goal and Call
// retain this same canonical format in their own bounded byte regions.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_BUSINESS_ENCODING_H
#define AYNI_CORE_CATALOG_CAPABILITY_BUSINESS_ENCODING_H
#include "catalog/capability/authority_projection.h"
#include "catalog/capability/call_contract_encoding.h"
#include "catalog/capability/schema_encoding.h"

namespace ayni::catalog {
constexpr uint32_t business_encoding_version = 1;
constexpr uint32_t business_encoding_bytes_max = 2U * schema_encoding_bytes_max + 4096U;
static_assert(business_encoding_bytes_max == AYNI_BUSINESS_CONTRACT_BYTES_MAX,
              "public original-proof bound matches the portable codec");

// Caller-owned decode scratch; schemas own their node tables, and all string
// views borrow the original complete proof. No current catalog is consulted.
struct DecodedBusiness {
    CallContract execution{}; // Original normalized execution semantics, never a live Provider.
    DecodedSchema input;      // Original input machine schema/defaults.
    DecodedSchema result;     // Original result machine schema/defaults.
    std::array<ayni_authority_projection_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> projections{};
    uint32_t count = 0; // Complete domain field mapping, zero for a declared empty domain.
    ayni_business_contract_v1 view() const {
        return {input.view(), result.view(), count, 0, count == 0U ? nullptr : projections.data()};
    }
};

// Execution was normalized in this boundary. Validates schemas/projection once,
// then compiles the full proof; capacity failure publishes only required size.
ayni_status_t encode_business(ayni_bytes_v1 domain, const CallContract &execution,
                              const ayni_business_contract_v1 &business, uint8_t *out,
                              uint32_t capacity, uint32_t &size);
// Registration already admitted execution and definition in this same call;
// finite authority cross references may be registered later and close before start.
ayni_status_t encode_admitted_business(const CallContract &execution,
                                       const ayni_business_contract_v1 &business, uint8_t *out,
                                       uint32_t capacity, uint32_t &size);
// Complete canonical machine proof, paired with its original authority domain.
// Failure preserves out; all decoded text borrows body until that body changes.
ayni_status_t decode_business(ayni_bytes_v1 domain, ayni_bytes_v1 body, DecodedBusiness &out);
// Copies an already canonical proof or compiles the same-call admitted definition
// into the receiving owner's unused slot. Nothing is published on failure.
ayni_status_t store_business_contract(const CallContract &execution,
                                      const ayni_business_contract_v1 *definition, uint8_t *out,
                                      uint32_t capacity, ayni_bytes_v1 &stored);
} // namespace ayni::catalog
#endif
