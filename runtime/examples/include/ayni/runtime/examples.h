// Public normal-path fixture composition for SDK examples, never a product deployment.
#ifndef AYNI_RUNTIME_EXAMPLES_H
#define AYNI_RUNTIME_EXAMPLES_H
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <memory>
namespace ayni::runtime::examples {
enum class Model { mock, alternate_loopback };
// Example adapter/policy lifecycle; every callback is owner-thread and nonthrowing.
struct Extension {
    void *context = nullptr;
    ayni_status_t (*configure)(void *, ayni_kernel_t *) =
        nullptr; // CONFIGURING, register public descriptors.
    ayni_status_t (*advance)(void *,
                             uint64_t) = nullptr; // Outside step; finite simulated microseconds.
    ayni_status_t (*quiesce)(void *) =
        nullptr; // Joined, contexts retained; null only with no external producers.
};
class Fixture {
  public:
    Fixture();
    ~Fixture(); // Failed close retains the complete composition/borrow contexts.
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ayni_status_t open(Extension extension, Model model = Model::mock);
    uint64_t instance() const; // Registered assistant instance; zero before open.
    uint64_t now_us() const;   // Same monotonic domain as the example Host.
    ayni_string_v1 capability(const char *alias) const; // Borrowed through close.
    ayni_scope_v1 scope(const char *alias) const; // Exact model destination, empty for local tools.
    ayni_status_t grant(ayni_string_v1 capability, ayni_scope_v1 scope);
    ayni_status_t create(const ayni_goal_create_v1 &, uint64_t &out_goal);
    ayni_status_t post(const ayni_event_v1 &); // Copied trusted simulated input.
    ayni_status_t step(); // Progress fixture adapters then one bounded Kernel step.
    ayni_status_t
    wait_task(uint64_t goal);        // Finite normal-path wait for one completed durable Task.
    uint32_t model_requests() const; // Actual alternate loopback HTTP requests, zero in mock mode.
    uint32_t notifications() const;  // Completed fixture-channel writes.
    ayni_status_t close(); // Bounded drain, joins producers, destroys Kernel, closes adapters.
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::runtime::examples
#endif
