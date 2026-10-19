/*
 * Ayni public C ABI (experimental, ABI 0.x).
 *
 * Only the functions declared in this header are implemented. Planned
 * interfaces live under include/ayni/draft/; they are compiled by the header
 * checks but have no symbols yet. Semantics are specified in contracts/.
 *
 * Rules that apply to every function here:
 * - Kernel functions are called on the kernel owner thread only.
 * - Inputs are copied before return unless a parameter says otherwise.
 * - Every ABI struct starts with struct_size, set to sizeof the caller's view
 *   of the struct (contracts/domain/versions.md). Fields are only appended
 *   within an ABI major. A struct_size below the first published layout of
 *   that major is AYNI_INVALID_ARGUMENT; one larger than this library knows
 *   means the caller was built against a newer minor and is
 *   AYNI_UNSUPPORTED_VERSION. A shorter struct from an older caller gets
 *   defaults for the fields it lacks.
 */
#ifndef AYNI_AYNI_H
#define AYNI_AYNI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A major change is incompatible and rejected by ayni_create; a minor change
 * only appends fields or functions. Major 0 means experimental. */
#define AYNI_ABI_MAJOR 0u
#define AYNI_ABI_MINOR 1u

/*
 * Status codes (contracts/domain/status_codes.md). 0 is success and negative
 * values are failures. A published value is never reused for another meaning;
 * callers treat an unknown negative value as a generic failure.
 */
typedef int32_t ayni_status_t;

#define AYNI_OK ((ayni_status_t)0)
#define AYNI_INVALID_ARGUMENT ((ayni_status_t)(-1))
#define AYNI_INVALID_STATE ((ayni_status_t)(-2))
#define AYNI_UNSUPPORTED_VERSION ((ayni_status_t)(-3))
#define AYNI_UNSUPPORTED ((ayni_status_t)(-4))
#define AYNI_NOT_READY ((ayni_status_t)(-5))
#define AYNI_BUSY ((ayni_status_t)(-6))
#define AYNI_CAPACITY_EXCEEDED ((ayni_status_t)(-7))
#define AYNI_BUFFER_TOO_SMALL ((ayni_status_t)(-8))
#define AYNI_NOT_FOUND ((ayni_status_t)(-9))
#define AYNI_DENIED ((ayni_status_t)(-10))
#define AYNI_EXPIRED ((ayni_status_t)(-11))
#define AYNI_STALE ((ayni_status_t)(-12))
#define AYNI_BUDGET_EXHAUSTED ((ayni_status_t)(-13))
#define AYNI_STORAGE_FAILED ((ayni_status_t)(-14))
#define AYNI_REVISION_CONFLICT ((ayni_status_t)(-15))
#define AYNI_IDEMPOTENCY_CONFLICT ((ayni_status_t)(-16))
#define AYNI_RESULT_UNKNOWN ((ayni_status_t)(-17))
#define AYNI_INTERNAL ((ayni_status_t)(-18))

/* Kernel lifecycle (contracts/domain/lifecycles.md, plan section 3.2). */
typedef uint32_t ayni_lifecycle_t;

#define AYNI_LIFECYCLE_CONFIGURING ((ayni_lifecycle_t)1)
#define AYNI_LIFECYCLE_RECOVERING ((ayni_lifecycle_t)2)
#define AYNI_LIFECYCLE_RUNNING ((ayni_lifecycle_t)3)
#define AYNI_LIFECYCLE_RESTRICTED ((ayni_lifecycle_t)4)
#define AYNI_LIFECYCLE_DRAINING ((ayni_lifecycle_t)5)
#define AYNI_LIFECYCLE_STOPPED ((ayni_lifecycle_t)6)

typedef struct ayni_kernel ayni_kernel_t;

typedef struct ayni_config_v1 {
    uint32_t struct_size;
    uint32_t abi_major; /* must equal AYNI_ABI_MAJOR */
} ayni_config_v1;

/*
 * Platform services the kernel uses to maintain its own invariants (ADR-0006).
 * ayni_create copies the table; ctx and the functions it points to stay owned
 * by the caller and must remain valid until ayni_destroy returns AYNI_OK.
 * Further services are appended as they are implemented; their planned shape
 * is in include/ayni/draft/host_v1.h.
 */
typedef struct ayni_host_v1 {
    uint32_t struct_size;
    void *ctx;
    /*
     * Required. Monotonic time in microseconds from an arbitrary per-boot
     * origin. Never decreases within a boot; not comparable across boots or
     * devices. Called on the kernel owner thread; must not block.
     */
    uint64_t (*monotonic_now_us)(void *ctx);
} ayni_host_v1;

/* Writes the ABI version this library implements. Callable from any thread. */
ayni_status_t ayni_get_abi_version(uint32_t *out_major, uint32_t *out_minor);

/*
 * Reports the size and alignment of the memory block that ayni_create needs
 * for this configuration. Core never allocates memory itself (ADR-0014).
 * *out_align is a power of two. Fails with AYNI_INVALID_ARGUMENT or
 * AYNI_UNSUPPORTED_VERSION exactly as ayni_create would for the same
 * configuration.
 */
ayni_status_t ayni_kernel_memory_requirements(const ayni_config_v1 *config, size_t *out_size,
                                              size_t *out_align);

/*
 * Creates a kernel in AYNI_LIFECYCLE_CONFIGURING inside caller-provided
 * memory. config and the host table are copied before return. The memory
 * block stays owned by the caller, who must neither use nor release it until
 * ayni_destroy returns AYNI_OK.
 *
 * Failures, with *out_kernel set to NULL (when out_kernel is not NULL) and
 * the memory left untouched:
 * - AYNI_INVALID_ARGUMENT: a null pointer, a struct_size shorter than the
 *   first published layout, a missing required host function, or memory not
 *   aligned as reported by ayni_kernel_memory_requirements;
 * - AYNI_UNSUPPORTED_VERSION: config->abi_major differs from AYNI_ABI_MAJOR,
 *   or a struct_size is larger than this library knows;
 * - AYNI_BUFFER_TOO_SMALL: memory_size is below the reported size.
 */
ayni_status_t ayni_create(const ayni_config_v1 *config, const ayni_host_v1 *host, void *memory,
                          size_t memory_size, ayni_kernel_t **out_kernel);

/* Writes the current lifecycle state. */
ayni_status_t ayni_get_lifecycle(const ayni_kernel_t *kernel, ayni_lifecycle_t *out_state);

/*
 * Ends the kernel's use of its memory and of the host functions and ctx.
 * Allowed only in CONFIGURING or STOPPED; otherwise returns
 * AYNI_INVALID_STATE and leaves the kernel unchanged. After AYNI_OK the handle
 * is invalid and the caller may release the memory block and ctx.
 */
ayni_status_t ayni_destroy(ayni_kernel_t *kernel);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AYNI_H */
