// Experiment-owned speaker Provider. Core owns Calls/authority; this owns I2S and PCM.
#pragma once
#include "protocol.h"
#include <atomic>
#include <ayni/calls.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <optional>
#include <string_view>
namespace wifichat {
class Transport;
class Speaker {
  public:
    static constexpr uint32_t pcm_rate = 16000, maximum_speech_ms = 30000;
    static constexpr uint32_t maximum_pcm_bytes =
        pcm_rate * maximum_speech_ms / 1000 * sizeof(int16_t);
    static constexpr uint32_t default_volume = 80;
    static constexpr uint32_t result_bytes = 192;
    static uint32_t input_bytes(); // Largest canonical input of both contracts.
    // Boot before other peripherals, or audio worker after deleting I2S only.
    // Hold BCLK/WS/DIN low so an idle MAX98357A cannot clock coupled noise.
    static esp_err_t park_outputs();
    explicit Speaker(Transport &transport) : transport_(transport) {}
    ayni_status_t configure(ayni_kernel_t *);
    // Owner-only readiness gate; retained Calls still drain/cancel while closed.
    void set_accepting(bool accepting) { accepting_ = accepting; }
    ayni_status_t advance(); // Owner only; never waits for I2S, synthesis or network I/O.
    bool receive(Kind, uint64_t, const uint8_t *, size_t); // I/O task; bounded copy and ACK.
    bool close(); // After Core STOPPED: joins the audio worker, retains PCM with the I/O context.
    uint32_t stack_free_bytes() const;
    // Owner-only stop for teardown/fenced sessions; still completes through Core.
    void request_stop();

  private:
    enum class Operation { tone, speech, quit };
    struct Job {
        Operation operation{};
        uint32_t frequency = 0, samples = 0, volume = 0;
    };
    struct Worker;
    struct Pending {
        Operation operation;
        uint64_t call, deadline;
        uint32_t volume;
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
    };
    enum class Transfer { none, begin, data, ready, failed };
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
    static void run(void *);
    bool initialize();
    bool enqueue(Job);
    ayni_status_t begin_speech(uint64_t id, std::string_view text);
    void request_cancel();
    void status(const char *);
    Transport &transport_;
    Worker *worker_ = nullptr; // Internal RAM; freed only after proven legacy-kernel task join.
    int16_t *pcm_ = nullptr;   // PSRAM lease from speech start through I2S completion/cancellation.
    std::optional<Pending> pending_; // Sole owner-thread completion borrow.
    std::atomic<bool> cancel_{false};
    portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    Transfer transfer_ = Transfer::none; // Guarded by mutex_, owner + I/O task only.
    uint64_t transfer_call_ = 0;
    uint32_t expected_ = 0, received_ = 0;
    bool queued_ = false;    // Owner only, exactly one worker submission per accepted Call.
    bool accepting_ = false; // Composition opens after session cleanup, closes before teardown.
    uint32_t final_stack_free_ = 0;
    ayni_schema_node_v1 tone_input_[4]{}, speech_input_[3]{}, result_{};
    ayni_value_node_v1 volume_default_node_{};
    ayni_value_v1 volume_default_{};
    ayni_business_contract_v1 businesses_[2]{};
    ayni_call_capability_v1 offers_[2]{};
    ayni_call_resource_v1 resource_{};
    ayni_provider_v1 provider_{};
};
} // namespace wifichat
