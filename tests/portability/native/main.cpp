#include "suite.h"
#include <ayni/hosts/native.h>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
namespace {
constexpr int64_t fixture_utc = 1893456000;
void utc(void *, ayni_utc_sample_v1 *out) {
    *out = {sizeof(*out), AYNI_UTC_TRUSTED, fixture_utc};
}
void yield(void *) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
} // namespace
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 2) {
        std::fprintf(stderr, "Usage: ayni-portable EXISTING_EMPTY_DIRECTORY\n");
        return 2;
    }
    std::printf("source=%s ABI=%u.%u\n", AYNI_SOURCE_REVISION, AYNI_ABI_MAJOR, AYNI_ABI_MINOR);
    ayni::runtime::portable::Memory memory;
    auto status = ayni::runtime::portable::Profile::memory_requirements(memory);
    if (status != AYNI_OK) {
        std::printf("query failed %d\n", status);
        return 1;
    }
    std::printf("portable memory %zu kernel %zu alignment %zu\n", memory.bytes, memory.kernel_bytes,
                memory.alignment);
    void *arena = ::operator new(memory.bytes, std::align_val_t(memory.alignment), std::nothrow);
    if (!arena) {
        return 1;
    }
    ayni::portability::Evidence evidence;
    for (uint32_t boot = 0; boot < 2; ++boot) {
        ayni::hosts::native::Host host({nullptr, utc});
        const auto opened = host.open_storage(argv[1], AYNI_COMPLETION_SLOT_COUNT_DEFAULT);
        if (opened.status != ayni::hosts::native::StorageStatus::ok) {
            return 1;
        }
        const auto ports = host.table();
        ayni::runtime::portable::Profile profile;
        status = profile.create_in(arena, memory.bytes, ports, host.value_store());
        if (status != AYNI_OK) {
            std::printf("create failed %d\n", status);
            host.close_storage();
            profile.destroy();
            return 1;
        }
        const ayni::portability::Clock clock{ports.ctx, ports.monotonic_now_us, yield};
        ayni::portability::Policies policies;
        status = ayni::portability::configure(profile, policies);
        if (status == AYNI_OK) {
            status = ayni::portability::start(profile, clock);
        }
        if (status == AYNI_OK) {
            status = boot == 0 ? ayni::portability::normal(profile, clock, evidence)
                               : ayni::portability::reopened(profile, clock, evidence);
        }
        const auto stopped = ayni::portability::stop(profile, clock);
        // Host worker may still be returning from its last post. Join before destroying/freeing
        // borrows.
        host.close_storage();
        const auto destroyed = profile.destroy();
        if (status != AYNI_OK || stopped != AYNI_OK || destroyed != AYNI_OK) {
            std::printf("failed status=%d stopped=%d destroyed=%d\n", status, stopped, destroyed);
            return 1;
        }
    }
    ::operator delete(arena, std::align_val_t(memory.alignment));
    return 0;
}
