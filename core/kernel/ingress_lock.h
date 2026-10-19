// The Host's ingress critical section (ADR-0015). It guards everything that
// threads other than the owner touch: the event queue, completion slot states
// and the posted-completion list. Hold it only for bounded copies and slot
// bookkeeping; call no other Host function while holding it.

#ifndef AYNI_CORE_KERNEL_INGRESS_LOCK_H
#define AYNI_CORE_KERNEL_INGRESS_LOCK_H

#include <ayni/ayni.h>

namespace ayni::kernel {

// Scoped hold of the ingress lock. host must have both lock functions; the
// kernel checks that when it is created.
class IngressGuard {
  public:
    explicit IngressGuard(const ayni_host_v1 &host) : host_(host) { host_.ingress_lock(host_.ctx); }
    ~IngressGuard() { host_.ingress_unlock(host_.ctx); }

    IngressGuard(const IngressGuard &) = delete;
    IngressGuard &operator=(const IngressGuard &) = delete;

  private:
    const ayni_host_v1 &host_;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INGRESS_LOCK_H
