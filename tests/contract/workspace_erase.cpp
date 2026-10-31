// ADR-0090 / C-SDK-007: the compiler must retain erasure even when the local
// bytes die on return. Observe the real call at link time, without changing it.
#include <array>
#include <ayni/detail/workspace.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
namespace {
constexpr std::size_t region_bytes = 37;
constexpr uint8_t private_byte = 73;
bool observed = false;
bool armed = false; // Initial zero-initialization may itself call memset on uninitialized bytes.
} // namespace
extern "C" void *__real_memset(void *, int, std::size_t);
extern "C" void *__wrap_memset(void *data, int value, std::size_t bytes) {
    if (armed && value == 0 && bytes == region_bytes &&
        static_cast<uint8_t *>(data)[0] == private_byte) {
        observed = true;
    }
    return __real_memset(data, value, bytes);
}
// Separate optimized function leaves the local region dead after this return.
[[gnu::noinline]] void erase_unobserved_local() {
    alignas(uint64_t) std::array<uint8_t, region_bytes> bytes{};
    ayni::detail::Workspace workspace(bytes.data(), bytes.size());
    auto lane = workspace.take(bytes.size());
    if (!lane) {
        std::abort();
    }
    static_cast<uint8_t *>(lane.get()->data)[0] = private_byte;
    armed = true; // Observe only destruction, after the private byte is initialized.
}
int main() {
    erase_unobserved_local();
    return observed ? 0 : 1;
}
