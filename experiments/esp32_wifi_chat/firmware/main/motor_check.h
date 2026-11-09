#pragma once
#include "motor_contract.h"
#include "protocol.h"
#include <ayni/strategies/builtin.h>
#include <cstring>
#include <optional>
namespace wifichat {
// Touchscreen-only fixed procedures. These do not add tools to the model directory.
struct MotorCheckNotice {
    enum class Phase : uint32_t { queued = 1, running, completed, failed, cancelled };
    static constexpr size_t wire_bytes = 4 * sizeof(uint32_t);
    Phase phase = Phase::queued;
    uint32_t motor = 0; // Zero-based motor, 0 or 1.
    ayni_status_t status = AYNI_OK;
    ayni_reason_t reason = AYNI_REASON_NONE;
    void encode(uint8_t *out) const;
    static bool decode(const uint8_t *, size_t, MotorCheckNotice &);
};
class MotorCheck {
  public:
    MotorCheck() = default;
    MotorCheck(const MotorCheck &) = delete;
    MotorCheck &operator=(const MotorCheck &) = delete;
    static constexpr uint32_t type_count = 2, percent = 50, duration_ms = 5000;
    struct SubmitResult {
        ayni_status_t status = AYNI_OK;
        ayni_reason_t reason = AYNI_REASON_NONE;
    };
    // Boot-lifetime owner-thread ports. Only notify may refuse queue admission;
    // stop bypasses the message queue and never waits for Core or the model.
    struct Owner {
        void *context = nullptr;
        bool (*ready)(const void *) = nullptr;
        SubmitResult (*submit)(void *, ayni_intent_kind_t, const void *, uint64_t *,
                               uint32_t) = nullptr;
        bool (*notify)(void *, uint64_t, const MotorCheckNotice &) = nullptr;
        void (*stop)(void *) = nullptr;
    };
    ayni_status_t configure(Owner);
    ayni_strategy_v1 strategy();
    ayni_status_t describe_type(uint32_t motor, ayni_goal_type_v1 &);
    ayni_status_t install(ayni_kernel_t *);
    static bool owns(ayni_string_v1, uint32_t version);
    ayni_status_t request(uint32_t motor, uint64_t request_id);
    void cancel();
    bool active() const { return run_.has_value(); }
    // Core queries are owner-only. This never performs I/O or emits a physical command itself.
    void tick(ayni_kernel_t *, uint64_t instance, uint64_t durable);

  private:
    static constexpr uint32_t field_count = 3, version = 1;
    struct Profile {
        std::array<ayni_scope_field_v1, field_count> fields{};
        std::array<ayni_value_node_v1, field_count + 1> input{};
        ayni::strategies::RuleStep step{};
    };
    enum class State { creating, waiting_task, running, cancelling, terminal };
    struct Run {
        State state = State::creating;
        uint32_t motor = 0;
        uint64_t request = 0, goal = 0;
        MotorCheckNotice notice{}; // Root failure survives cancellation; terminal waits for UI.
    };
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static uint32_t ready(void *, uint64_t);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
    static ayni_status_t validate(void *, ayni_bytes_v1);
    void create(uint64_t instance);
    void poll(ayni_kernel_t *, uint64_t durable);
    void cancel_goal(const ayni_goal_info_v1 &);
    void fail(ayni_status_t, ayni_reason_t);
    void finish(MotorCheckNotice::Phase);
    void publish();
    Owner owner_{};
    std::array<Profile, type_count> profiles_{};
    std::array<ayni::strategies::Rule, type_count> rules_{};
    ayni::strategies::Rules policy_;
    ayni_strategy_v1 inner_{};
    std::optional<Run> run_;
};
} // namespace wifichat
