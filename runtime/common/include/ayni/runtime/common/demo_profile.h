// P2 shared Native/Sim deployment (ADR-0057). Concrete components live here;
// applications submit only public intents. All methods use the Kernel owner thread.
#ifndef AYNI_RUNTIME_DEMO_PROFILE_H
#define AYNI_RUNTIME_DEMO_PROFILE_H
#include <ayni/applications/assistant.h>
#include <ayni/authority.h>
#include <ayni/ayni.h>
#include <ayni/providers/presentation.h>
#include <ayni/runtime/common/clock.h>
#include <memory>
#include <string>
namespace ayni::runtime {
inline constexpr uint64_t demo_owner = 1;
inline constexpr uint64_t demo_agent_a = 1;
inline constexpr uint64_t demo_agent_b = 2;
inline constexpr uint32_t demo_binding_revision = 1;
enum class Model { mock, remote };
// Runtime configuration contains the environment variable NAME, never the key.
struct ModelConfig {
    Model model = Model::mock;
    std::string base_url;
    std::string model_name;
    std::string key_environment = "AYNI_LLM_API_KEY";
};
// Runtime owns concrete wiring. An explicit sink is required for notification evidence;
// tests/Sim may supply a deterministic sink, while the CLI supplies checked write+flush.
struct AssistantPorts {
    Clock clock;
    providers::presentation::Output output;
    applications::assistant::QuietHours quiet;
};
ayni_config_v1 demo_config();
class DemoProfile {
  public:
    DemoProfile();
    ~DemoProfile();
    DemoProfile(const DemoProfile &) = delete;
    DemoProfile &operator=(const DemoProfile &) = delete;
    // Configures the providers and immutable policies, then registers all objects.
    // On failure retain the profile until the partially configured Kernel is destroyed.
    ayni_status_t assemble(ayni_kernel_t *kernel, const ModelConfig &config, AssistantPorts ports);
    ayni_status_t advance(); // Deliver deterministic Mock completions / poll transport health.
    ayni_status_t quiesce(); // STOPPED Kernel: join producers, preserve registered contexts.
    ayni_status_t close();   // Only after Kernel destruction; joins remote workers.
    // Trusted aliases: model, summary, present (legacy Mock), notify, health, observe;
    // unknown returns empty. Views are
    // borrowed until close. Scope names the exact configured remote destination.
    ayni_string_v1 capability(const std::string &alias) const;
    // Actual configured model destination; empty for local Mock.
    ayni_string_v1 destination() const;
    ayni_scope_v1 scope(const std::string &alias) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::runtime
#endif
