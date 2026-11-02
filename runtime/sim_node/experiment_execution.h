// One deterministic physical deployment profile. Core remains the only issuer;
// this owner controls a simulated transport, local measurements and fake time.
#ifndef AYNI_EXPERIMENT_EXECUTION_H
#define AYNI_EXPERIMENT_EXECUTION_H
#include <ayni/providers/mock_execution.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/scenes.h>
#include <optional>
namespace ayni::runtime::experiments {
class World;
class Physical {
  public:
    static constexpr uint32_t lease_us = 100000;
    static constexpr uint32_t observation_age_us = 500000;
    explicit Physical(World &world);
    bool configure();
    CallBytes call_bytes() const; // The execution contract's maxima.
    bool register_profile(ayni_kernel_t *kernel);
    bool prepare(uint64_t agent = 1);
    bool revoke_queued();
    bool revoke_running();
    bool unauthorized();
    bool stop_full_queue();
    bool quarantine();
    bool publish(int64_t value);
    bool advance();
    ayni_status_t close();       // After Kernel destruction, await independent stop and release.
    bool at(uint32_t offset_us); // Since the first actual permit's nonce issue time.
    bool elapse(uint32_t duration_us); // Local cycles, without Core steps.
    bool mark_invalidated();
    bool stopped_within_bound();
    bool invalidate_kernel();
    bool invalidate_calibration();
    bool estop();
    bool estop_before_dispatch();
    bool reset_estop();
    bool replay_permit_rejected();
    bool replay_renewal_rejected();
    bool unpermitted_rejected();
    bool deliver_renewal(ayni_status_t expected);
    bool drop_renewal();
    bool power_cycle();
    uint64_t deadline_offset() const;
    ayni_control_info_v1 info() const;
    providers::mock_execution::Executor executor;
    uint32_t scene_version = 1; // Catalog choice only while no Kernel exists.

  private:
    World &world_;
    std::optional<uint64_t> nonce_origin_, invalidated_at_;
    struct StoppedOutput {
        uint64_t permit_id;
        uint64_t samples;
    };
    std::optional<StoppedOutput> stopped_output_; // Diagnostic total scoped to the measured permit.
    uint64_t observation_sequence_ = 0;
    enum class Invalidation { kernel, calibration, estop };
    bool change_live(Invalidation kind);
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
};
} // namespace ayni::runtime::experiments
#endif
