// How CommandTable::find_exact reads an exact command's request (C-WIRE-017,
// ADR-0030). Each request type names its intent kind and canonical codec in a
// specialization beside the codec, so the table keeps one lookup and a new
// owner command adds a specialization rather than a lookup overload.
// Revocation is not one of these: its lookup only reads a pending control and
// never stages bytes (ADR-0031).

#ifndef AYNI_CORE_KERNEL_COMMAND_EXACT_COMMAND_H
#define AYNI_CORE_KERNEL_COMMAND_EXACT_COMMAND_H

namespace ayni::kernel {

// A specialization provides, for its Request:
//   static ayni_intent_kind_t kind(const Request &);
//   measure: ayni_status_t (*)(const Request &, uint32_t &size)
//   encode:  ayni_status_t (*)(const Request &, uint8_t *out, uint32_t capacity, uint32_t &size)
// measure validates the complete shape; encode writes exactly the measured bytes.
template <typename Request> struct ExactCommand;

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_EXACT_COMMAND_H
