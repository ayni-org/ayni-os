// The execution contract's one portable field layout. Catalog owns these
// semantics; Call records and authority business proofs both consume this codec.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_CALL_CONTRACT_ENCODING_H
#define AYNI_CORE_CATALOG_CAPABILITY_CALL_CONTRACT_ENCODING_H
#include "catalog/capability/call_contract.h"
#include "foundation/record_body.h"

namespace ayni::catalog {
constexpr uint32_t execution_contract_word_count = 13;
constexpr uint32_t execution_contract_fixed_bytes =
    execution_contract_word_count * sizeof(uint32_t);
// Excludes optional subscription source semantics, carried by its enclosing owner.
uint32_t execution_contract_size(const CallContract &contract);
// Representation v1, matching CALL_STATE's original order in records.md.
void write_execution_contract(const CallContract &contract, foundation::BodyWriter &writer);
// Reads framing once; the enclosing fact's shared validator checks complete semantics.
bool read_execution_contract(foundation::BodyReader &reader, CallContract &contract);
} // namespace ayni::catalog
#endif
