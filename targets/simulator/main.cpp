// ayni-sim: native executable over runtime/ (ADR-0010). In M0 it proves the
// empty Core assembles, reports its lifecycle and shuts down. Experiment
// runners (--experiment E1..E4) arrive with the M1 fixtures.

#include <ayni/ayni.h>
#include <ayni/runtime/native_node.h>

#include <cinttypes>
#include <cstdio>

namespace {

const char *lifecycle_name(ayni_lifecycle_t state) {
    switch (state) {
    case AYNI_LIFECYCLE_CONFIGURING:
        return "CONFIGURING";
    case AYNI_LIFECYCLE_RECOVERING:
        return "RECOVERING";
    case AYNI_LIFECYCLE_RUNNING:
        return "RUNNING";
    case AYNI_LIFECYCLE_RESTRICTED:
        return "RESTRICTED";
    case AYNI_LIFECYCLE_DRAINING:
        return "DRAINING";
    case AYNI_LIFECYCLE_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

int fail(const char *step, ayni_status_t status) {
    std::fprintf(stderr, "ayni-sim: %s failed with status %" PRId32 "\n", step, status);
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        std::fprintf(stderr, "ayni-sim: unsupported argument '%s'; M0 takes no options\n", argv[1]);
        return 2;
    }

    uint32_t abi_major = 0;
    uint32_t abi_minor = 0;
    ayni_status_t status = ayni_get_abi_version(&abi_major, &abi_minor);
    if (status != AYNI_OK) {
        return fail("ayni_get_abi_version", status);
    }
    std::printf("ayni-sim: ABI %" PRIu32 ".%" PRIu32 " (experimental)\n", abi_major, abi_minor);

    ayni::runtime::NativeNode node;
    status = node.assemble();
    if (status != AYNI_OK) {
        return fail("assemble", status);
    }

    ayni_lifecycle_t lifecycle = 0;
    status = ayni_get_lifecycle(node.kernel(), &lifecycle);
    if (status != AYNI_OK) {
        return fail("ayni_get_lifecycle", status);
    }
    std::printf("ayni-sim: kernel created, lifecycle %s\n", lifecycle_name(lifecycle));

    status = node.release();
    if (status != AYNI_OK) {
        return fail("release", status);
    }
    std::printf("ayni-sim: kernel destroyed\n");
    return 0;
}
