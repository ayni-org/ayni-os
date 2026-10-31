// Source SDK composition profile shared by independent Native and ESP consumers.
#ifndef AYNI_RUNTIME_PORTABLE_H
#define AYNI_RUNTIME_PORTABLE_H
#include <ayni/ayni.h>
#include <ayni/runtime/conversation.h>
namespace ayni::runtime::portable {
inline constexpr uint32_t agent_count = 2;
inline constexpr uint32_t live_task_limit = 4;
inline constexpr uint64_t owner_principal = 1;
// Same fixed capacities on both platforms; this is a deployment profile, not Core defaults.
ayni_config_v1 configuration();
struct ExecutionEvidence {
    uint64_t call = 0;        // Last stable simulated physical call, zero before submission.
    uint64_t samples = 0;     // Outputs accepted by the independent permit gate.
    uint64_t submissions = 0; // Accepted Provider operations, not inferred from text.
};
struct Memory {
    size_t bytes = 0; // Caller-owned Impl/Kernel/workspace/conversation region; excludes Host and
                      // Provider heaps.
    size_t alignment = 0; // Power of two; platform chooses the actual allocation/placement.
    size_t kernel_bytes = 0;
};
// One owner. No platform allocation/Host selection here; the platform composition
// supplies a fully opened Host. All accepted workers/Providers must be joined
// before destroy/free; this object never frees a still-borrowed region.
class Profile {
  public:
    Profile() = default;
    Profile(const Profile &) = delete;
    Profile &operator=(const Profile &) = delete;
    static ayni_status_t memory_requirements(Memory &);
    // CONFIGURING on success, allowing the consumer to register its own application.
    // Existing Providers make bounded nothrow startup allocations outside this region.
    // On partial registration/allocation failure, retain the region; destroy after Host join.
    ayni_status_t create_in(void *, size_t, const ayni_host_v1 &, hosts::storage::FileValuePort);
    ayni_status_t start();
    // Progresses concrete fixture Providers outside the bounded Core step, then Core and app.
    // No waiting; caller's OS loop owns wake/yield and its overall deadline.
    ayni_status_t tick();
    bool running() const; // RUNNING and a durable authenticated management session exists.
    ayni_status_t open_conversations(); // Repeated owner call, BUSY until both exact files ready.
    runtime::conversation::Session *conversation(uint32_t index);
    ayni_kernel_t *kernel() const;
    uint64_t session() const;
    uint64_t instance(uint32_t index) const; // Stable Kernel Agent instance; 0 before RUNNING.
    // Same scratch as strategies/codecs: owner only, outside tick/Session calls
    // and all active callbacks. It is not an independent concurrent region.
    const ayni_workspace_v1 *value_workspace() const;
    Submission submit(ayni_intent_kind_t, const void *, uint32_t revision = 0);
    bool command_pending() const; // One accepted command receipt still awaiting durability.
    ayni_status_t begin_shutdown();
    bool stopped() const;
    // After STOPPED (or CONFIGURING), then Host close/join. Refuses live Provider borrows.
    ayni_status_t destroy();
    uint64_t model_executions(uint32_t index) const;
    ExecutionEvidence execution_evidence() const;

  private:
    struct Impl;
    Impl *impl_ = nullptr; // Borrowed caller region, retained after any partial creation.
};
} // namespace ayni::runtime::portable
#endif
