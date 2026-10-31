// Kernel owners use the same bounded lexical allocator as portable strategies.
#ifndef AYNI_CORE_FOUNDATION_WORKSPACE_H
#define AYNI_CORE_FOUNDATION_WORKSPACE_H
#include <ayni/detail/workspace.hpp>
namespace ayni::foundation {
using Workspace = ::ayni::detail::Workspace;
} // namespace ayni::foundation
#endif
