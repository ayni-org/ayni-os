// Owner-only projection from durable subscription coordination to worker gates.
// Transport owns synchronization and sink addresses; the service owns Call facts.
#ifndef AYNI_CORE_KERNEL_PORTS_SUBSCRIPTION_INGRESS_PORT_H
#define AYNI_CORE_KERNEL_PORTS_SUBSCRIPTION_INGRESS_PORT_H
#include <ayni/subscriptions.h>
namespace ayni::kernel {
// Three operations cross the state-owner boundary without a service -> runtime dependency.
struct SubscriptionIngressPort {
    void *context; // Borrowed transport owner, alive through all CallRuntime operations.
    ayni_subscription_sink_t *(*prepare)(
        void *, uint32_t, ayni_subscription_ref_v1, uint32_t,
        uint64_t); // Install closed binding after old borrow drained.
    bool (*activate)(void *, uint32_t, ayni_subscription_ref_v1,
                     uint64_t);                                // Confirm exact finite lease.
    void (*close)(void *, uint32_t, ayni_subscription_ref_v1); // Refuse new posts immediately.
};
} // namespace ayni::kernel
#endif
