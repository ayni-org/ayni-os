// ADR-0067: public C++ wrapper is self-contained without exceptions or RTTI.
#include <ayni/ayni.hpp>
#include <type_traits>
static_assert(!std::is_copy_constructible<ayni::Kernel>::value, "unique owner");
static_assert(std::is_nothrow_move_constructible<ayni::Kernel>::value, "borrow transfer");
static_assert(!std::is_move_assignable<ayni::Kernel>::value, "no replacement of live arenas");
int main() {
    ayni::Kernel kernel;
    const auto body = ayni::initialized<ayni_goal_create_v1>();
    const auto result = kernel.submit_raw(AYNI_INTENT_GOAL_CREATE, 1, 1, 0, body);
    return kernel.get() == nullptr && kernel.close() == AYNI_OK &&
                   result.status == AYNI_INVALID_STATE && result.error.status == result.status
               ? 0
               : 1;
}
