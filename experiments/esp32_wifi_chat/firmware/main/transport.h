#pragma once
#include "barge_in.h"
#include "control_mailbox.h"
#include "diagnostic_log.h"
#include "display.h"
#include "model_handoff.h"
#include "protocol.h"
#include "speech_profile.h"
#include <array>
#include <atomic>
#include <ayni/bodies.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
namespace wifichat {
class Speaker;
// Fixed PSRAM lanes; send never waits or allocates. The I/O task owns touchscreen and Wi-Fi
// orchestration.
class Transport {
  public:
    explicit Transport(ControlMailbox &control) : control_(control) {}
    bool initialize();
    DiagnosticLog &diagnostics() { return diagnostics_; }
    bool send(Kind, uint64_t, const void *, size_t);
    bool receive(Packet &);
    // Best-effort progress only. Lifecycle and turn terminals keep the reliable send lane.
    bool send_latest_status(uint64_t, const void *, size_t);
    // Only the API worker publishes real model completions. Success copies all payload bytes.
    ModelPost post_model(const Packet &, ModelDeliveryTiming);
    uint32_t api_generation() const { return model_.generation(); }
    void cancel_api_jobs() { model_.cancel(); }
    AudioWrite append_audio(uint64_t, uint32_t, const uint8_t *, size_t);
    // In-process backend boundary. Both queues copy and return immediately; only
    // a backend worker calls post/take_output. Audio retains the Speaker's lock.
    bool post(Kind, uint64_t, const void *, size_t);
    bool take_output(Packet &);
    bool paint(uint64_t call, const uint8_t *pixels, size_t bytes, uint32_t pages = 0);
    bool display_allowed(uint64_t call) const {
        return call && call == display_call_.load(std::memory_order_acquire);
    }
    Display &local_display() { return display_; }
    void allow_display(uint64_t call) { display_call_.store(call, std::memory_order_release); }
    void attach_speaker(Speaker *speaker) { speaker_.store(speaker, std::memory_order_release); }
    void attach_bodies(const ayni_body_store_v1 *bodies) {
        bodies_.store(bodies, std::memory_order_release);
    }
    const ayni_body_store_v1 *bodies() const { return bodies_.load(std::memory_order_acquire); }
    void run();
    ControlMailbox &control_mailbox() { return control_; }
    bool submit_motor_target(ControlTarget target, uint64_t *seq = nullptr);
    bool request_motor_hold(uint32_t percent);
    // Atomic configuration snapshot; default hold at 25%, no language parsing.
    bool set_barge_in_policy(BargeInPolicy, uint32_t hold_percent = default_hold_scale_percent);
    bool apply_barge_in();
    void set_motor_active(bool active) { motor_active_.store(active, std::memory_order_release); }
    // Startup before the I/O task; context has the composition's boot lifetime.
    void attach_motor_wake(void *context, void (*wake)(void *)) {
        motor_context_ = context;
        wake_motor_ = wake;
    }
    void request_motor_stop();
    void request_motor_soft_stop();
    void request_motor_disable() { motor_disable_.store(true, std::memory_order_release); }
    bool motor_disable_requested() {
        return motor_disable_.exchange(false, std::memory_order_acq_rel);
    }

  private:
    DiagnosticLog diagnostics_;
    Display display_;
    ModelHandoff model_;
    void *motor_context_ = nullptr;
    void (*wake_motor_)(void *) = nullptr;
    std::atomic<bool> motor_active_{false};
    static constexpr uint32_t policy_shift = 8, policy_mask = 255;
    std::atomic<uint32_t> barge_in_{(uint32_t(BargeInPolicy::hold) << policy_shift) |
                                    default_hold_scale_percent};
    ControlMailbox &control_; // Composition-owned internal RAM on ESP; boot lifetime.
    static constexpr size_t progress_bytes = 512;
    LatestSlot<BasicPacket<progress_bytes>> progress_;
    std::atomic<bool> motor_disable_{false}; // Ordinary disarm also bypasses queue capacity.
    QueueHandle_t input_free_ = nullptr, output_free_ = nullptr;
    QueueHandle_t output_ = nullptr, input_ = nullptr;
    std::array<InputPacket, input_packet_slots> inputs_{};
    std::array<OutputPacket, output_packet_slots> outputs_{};
    std::atomic<uint64_t> display_call_{0}; // Only the current authorized document may paint.
    std::atomic<Speaker *> speaker_{
        nullptr}; // Frozen composition, alive through I/O task lifetime.
    std::atomic<const ayni_body_store_v1 *> bodies_{nullptr}; // Stable descriptor, boot lifetime.
};
} // namespace wifichat
