#pragma once
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
    bool initialize();
    DiagnosticLog &diagnostics() { return diagnostics_; }
    bool send(Kind, uint64_t, const void *, size_t);
    bool receive(Packet &);
    // Only the API worker publishes real model completions. Success copies all payload bytes.
    ModelPost post_model(const Packet &, ModelDeliveryTiming);
    uint32_t api_generation() const { return model_.generation(); }
    void cancel_api_jobs() { model_.cancel(); }
    AudioWrite append_audio(uint64_t, uint32_t, const uint8_t *, size_t);
    // In-process backend boundary. Both queues copy and return immediately; only
    // a backend worker calls post/take_output. Audio retains the Speaker's lock.
    bool post(Kind, uint64_t, const void *, size_t);
    bool take_output(Packet &);
    bool paint(uint64_t call, const uint8_t *pixels, size_t bytes);
    Display &local_display() { return display_; }
    void allow_display(uint64_t call) { display_call_.store(call, std::memory_order_release); }
    void attach_speaker(Speaker *speaker) { speaker_.store(speaker, std::memory_order_release); }
    void attach_bodies(const ayni_body_store_v1 *bodies) {
        bodies_.store(bodies, std::memory_order_release);
    }
    const ayni_body_store_v1 *bodies() const { return bodies_.load(std::memory_order_acquire); }
    void run();
    bool motor_stop_requested() { return motor_stop_.exchange(false, std::memory_order_acq_rel); }
    void set_motor_active(bool active) { motor_active_.store(active, std::memory_order_release); }
    void request_motor_stop() { motor_stop_.store(true, std::memory_order_release); }
    void request_motor_disable() { motor_disable_.store(true, std::memory_order_release); }
    bool motor_disable_requested() {
        return motor_disable_.exchange(false, std::memory_order_acq_rel);
    }

  private:
    DiagnosticLog diagnostics_;
    Display display_;
    ModelHandoff model_;
    std::atomic<bool> motor_active_{false};
    std::atomic<bool> motor_stop_{false};    // Stop bypasses bounded message queues.
    std::atomic<bool> motor_disable_{false}; // Ordinary disarm also bypasses queue capacity.
    QueueHandle_t free_ = nullptr, output_ = nullptr, input_ = nullptr;
    std::array<Packet, packet_slots> packets_{};
    std::atomic<uint64_t> display_call_{0}; // Only the current authorized document may paint.
    std::atomic<Speaker *> speaker_{
        nullptr}; // Frozen composition, alive through I/O task lifetime.
    std::atomic<const ayni_body_store_v1 *> bodies_{nullptr}; // Stable descriptor, boot lifetime.
};
} // namespace wifichat
