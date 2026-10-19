/* Uses the implemented ABI from strict C99 with caller-provided memory. */

#include <ayni/ayni.h>

#include <stdio.h>

static uint64_t test_clock(void *ctx) {
    (void)ctx;
    return 1u;
}

/* Single-threaded, so wake and the ingress lock have nothing to do. */
static void test_wake(void *ctx) {
    (void)ctx;
}
static void test_lock(void *ctx) {
    (void)ctx;
}
static void test_unlock(void *ctx) {
    (void)ctx;
}

/* C99 has no alignas; a union of the widest scalar types gives the strictest
 * fundamental alignment, which the reported requirement must not exceed. */
typedef union {
    long double ld;
    uint64_t u64;
    void *ptr;
    void (*fn)(void);
} max_aligned_t;

static max_aligned_t kernel_memory[64];

static int expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "c_abi_smoke: %s\n", what);
    }
    return condition ? 0 : 1;
}

int main(void) {
    ayni_config_v1 config = {0};
    ayni_host_v1 host = {0};
    ayni_kernel_t *kernel = NULL;
    ayni_lifecycle_t state = 0;
    size_t size = 0;
    size_t align = 0;
    int failures = 0;

    config.struct_size = (uint32_t)sizeof config;
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 2u;
    config.event_body_max_bytes = 8u;
    config.completion_slot_count = 1u;
    host.struct_size = (uint32_t)sizeof host;
    host.monotonic_now_us = test_clock;
    host.wake = test_wake;
    host.ingress_lock = test_lock;
    host.ingress_unlock = test_unlock;

    failures += expect(ayni_kernel_memory_requirements(&config, &size, &align) == AYNI_OK,
                       "memory requirements");
    failures += expect(size <= sizeof kernel_memory, "kernel fits the static block");
    failures += expect(align <= sizeof(max_aligned_t) && sizeof(max_aligned_t) % align == 0,
                       "static block alignment suffices");
    if (failures != 0) {
        return 1;
    }

    failures +=
        expect(ayni_create(&config, &host, kernel_memory, sizeof kernel_memory, &kernel) == AYNI_OK,
               "create");
    failures +=
        expect(ayni_get_lifecycle(kernel, &state) == AYNI_OK && state == AYNI_LIFECYCLE_CONFIGURING,
               "lifecycle is CONFIGURING");
    failures += expect(ayni_completion_post(NULL, NULL) == AYNI_INVALID_ARGUMENT,
                       "completion post links and rejects NULL");
    failures += expect(ayni_destroy(kernel) == AYNI_OK, "destroy");
    return failures == 0 ? 0 : 1;
}
