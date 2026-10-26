// Owner-thread callback boundary shared by ABI admission and domain services.
// The Kernel owns the gate; borrowing services cannot reset its state (ADR-0058).
#ifndef AYNI_CORE_KERNEL_PORTS_CALLBACK_GATE_H
#define AYNI_CORE_KERNEL_PORTS_CALLBACK_GATE_H

namespace ayni::kernel {

class CallbackGate {
  public:
    // True only while a caller callback or an enclosing callback scope runs.
    bool active() const { return active_; }
    // Mark precisely caller code, preserving nested scopes and every return path.
    template <typename Call> auto invoke(Call &&call) -> decltype(call()) {
        Scope scope(*this);
        return call();
    }

  private:
    // Restores the previously active scope; cannot escape or be copied.
    class Scope {
      public:
        explicit Scope(CallbackGate &gate) : gate_(gate), enclosing_(gate.active_) {
            gate_.active_ = true;
        }
        ~Scope() { gate_.active_ = enclosing_; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

      private:
        CallbackGate &gate_; // Borrowed owner boundary, outlives this synchronous scope.
        bool enclosing_;     // Its entry state; nested calls must not clear an outer callback.
    };
    bool active_ = false; // No callback at construction; Scope is its sole writer.
};

} // namespace ayni::kernel
#endif
