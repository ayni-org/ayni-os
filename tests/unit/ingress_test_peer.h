// White-box setup for the u64 resource boundary; behavior still goes through
// ayni_post_event. No generic setter or test-only runtime API is introduced.
#ifndef AYNI_TESTS_UNIT_INGRESS_TEST_PEER_H
#define AYNI_TESTS_UNIT_INGRESS_TEST_PEER_H

#include "kernel/ports/ingress_lock.h"
#include "kernel/runtime/ingress/event_ingress.h"

namespace ayni::kernel {
class EventIngressTestPeer {
  public:
    static void exhaust_input_order(EventIngress &ingress) {
        const IngressGuard guard(ingress.host_);
        ingress.accepted_.through = UINT64_MAX;
    }
};
} // namespace ayni::kernel
#endif // AYNI_TESTS_UNIT_INGRESS_TEST_PEER_H
