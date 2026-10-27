// Existing Native protocol tests share the normal loopback fixture with SDK examples.
#ifndef AYNI_TEST_LOOPBACK_HTTP_H
#define AYNI_TEST_LOOPBACK_HTTP_H
#include <ayni/providers/openai_compat/loopback.h>
namespace ayni::test {
using LoopbackHttp = providers::openai_compat::fixtures::LoopbackHttp;
}
#endif
