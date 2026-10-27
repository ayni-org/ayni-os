// Trusted assistant policies and projections; Kernel remains the work/authority owner.
#ifndef AYNI_APPLICATIONS_ASSISTANT_H
#define AYNI_APPLICATIONS_ASSISTANT_H
#include <array>
#include <ayni/calls.h>
#include <ayni/memory.h>
#include <string>
#include <vector>
namespace ayni::applications::assistant {
inline constexpr uint32_t autonomous_group = 1;
inline constexpr uint32_t text_bytes_max = 384;
inline constexpr uint32_t memory_sources_max = 4;
inline constexpr uint32_t parameter_bytes_max = 560;
inline constexpr uint64_t check_period_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US;
enum class GoalKind { reminder, device_check, memory_organize, scene_observe };
// Immutable input. Sources are explicit selected memory IDs; no implicit export.
struct Parameters {
    GoalKind kind = GoalKind::reminder;
    std::string text;
    std::vector<uint64_t> sources;
};
struct Encoded {
    std::array<uint8_t, parameter_bytes_max> bytes{};
    uint32_t size = 0;
    ayni_bytes_v1 view() const { return {bytes.data(), size}; }
};
ayni_status_t encode(const Parameters &, Encoded &);
ayni_status_t decode(ayni_bytes_v1, Parameters &);
const char *goal_type(GoalKind);
// Encoded storage must outlive submission; Core copies the request synchronously.
ayni_status_t prepare_goal(const Parameters &, uint64_t owner_instance, uint64_t period_us,
                           const ayni_calendar_rule_v1 *, Encoded &, ayni_goal_create_v1 &);
// Explicit mentions win; check/检查 selects A, other ordinary text selects B.
uint64_t route(const std::string &, uint64_t agent_a, uint64_t agent_b, std::string &body);
// Civil interval [start,end), possibly crossing midnight; equal endpoints are invalid.
struct QuietHours {
    bool enabled = false;
    uint32_t start_minute = 0, end_minute = 0;
    std::string timezone;
    ayni_status_t validate() const;
    ayni_status_t allows(ayni_utc_sample_v1, bool &out) const;
};
// Borrowed provider operands, fixed through Kernel destruction by the composition root.
struct PolicyConfig {
    ayni_scope_v1 model_scope{sizeof(ayni_scope_v1), 0, nullptr};
    ayni_string_v1 model_capability{}, present_capability{}, health_capability{},
        observe_capability{};
    void *clock_context = nullptr;
    void (*utc_now)(void *, ayni_utc_sample_v1 *) = nullptr;
};
class Goals {
  public:
    Goals() = default;
    Goals(const Goals &) = delete;
    Goals &operator=(const Goals &) = delete;
    // Registers one policy and four GoalTypes; partial failure keeps ctx borrowed.
    ayni_status_t register_all(ayni_kernel_t *, PolicyConfig);

  private:
    struct TypeContext {
        Goals *owner = nullptr;
        GoalKind kind = GoalKind::reminder;
    };
    std::array<TypeContext, 4> types_{};
    PolicyConfig config_{};
    bool configured_ =
        false; // First registration attempt fixes borrowed configuration permanently.
    static ayni_status_t validate(void *, ayni_bytes_v1);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
};
enum class Presentation { available, presenting, presented, unknown, failed };
Presentation presentation(const ayni_call_info_v1 &);
enum class State {
    available,
    processing,
    observing,
    awaiting_authority,
    paused,
    offline,
    needs_attention
};
// Read-only aggregate, never another owner of work or authority.
struct Activity {
    State primary = State::available;
    uint32_t processing = 0, observing = 0, awaiting_authority = 0, needs_attention = 0;
    bool autonomy_paused = false;
};
ayni_status_t project(const ayni_kernel_t *, Activity &);
// Exact duplicates share evidence; conflicting values remain separate for owner review.
struct Suggestion {
    std::string key, value;
    uint32_t agents = 0;
    bool conflict = false;
};
std::vector<Suggestion> merge_suggestions(const std::vector<Suggestion> &);
} // namespace ayni::applications::assistant
#endif
