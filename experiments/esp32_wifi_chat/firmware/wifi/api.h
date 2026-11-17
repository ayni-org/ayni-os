#pragma once
#include "../main/tool_outline.h"
#include "../main/transport.h"
#include "capture.h"
#include "context_probe.h"
#include "settings.h"
#include <array>
#include <atomic>
#include <freertos/queue.h>
#include <freertos/task.h>
namespace wifichat::wifi {
class SpeechStream;
enum class Request { model, speech, recognize, health, capacity, template_capture };
// Three owned job slots, one worker. Queues contain indices, never large packets
// or borrowed pointers. Results retain a slot until the UI explicitly releases it.
class Api {
  public:
    static constexpr size_t slots = 3;
    struct Job {
        Request request{};
        Packet input{}, output{};
        ToolOutline outline; // Optional metadata only, generated before the decoded Answer dies.
        uint32_t generation = 0;
        int64_t deadline = 0;     // Monotonic microseconds, includes local queue wait.
        uint8_t *audio = nullptr; // ASR ownership transfers only after successful submit.
        size_t audio_bytes = 0;
        uint8_t *response = nullptr; // Worker allocation; result owns until release.
        size_t response_bytes = 0;
        bool ok = false;
        std::array<char, 128> error{};
    };
    struct ModelPreview {
        uint64_t id = 0;
        uint32_t generation = 0, size = 0;
        std::array<char, answer_bytes> text{};
    };
    bool take_preview(ModelPreview &);
    bool initialize(const Settings &settings, Transport &transport);
    bool submit(Request kind, uint64_t id, const void *data = nullptr, size_t bytes = 0,
                uint8_t *audio = nullptr, size_t audio_bytes = 0);
    // UI may close borrowed stores when true; completed results can still own buffers.
    bool idle() const { return outstanding_.load(std::memory_order_acquire) == 0; }
    Job *result();
    void release(Job *);
    void cancel_all() { generation_.fetch_add(1, std::memory_order_acq_rel); }
    void cancel_speech(uint64_t id) { speech_cancel_.store(id, std::memory_order_release); }
    bool current(const Job &job) const;

  private:
    static void *work(void *);
    void perform(Job &);
    void model(Job &, const char *url);
    void speech(Job &, const char *url);
    void recognize(Job &, const char *url);
    void capacity(Job &);
    ayni::providers::model_protocol::RequestOptions model_options(bool tools_allowed) const;
    bool http(Job &, const char *url, const char *content_type, const void *prefix,
              size_t prefix_bytes, const void *body, size_t body_bytes, const void *suffix,
              size_t suffix_bytes, size_t response_limit, CaptureReader *capture = nullptr,
              SpeechStream *stream = nullptr,
              ayni::providers::model_protocol::Stream *model_stream = nullptr);
    Settings settings_{};
    ContextProbe context_probe_;     // API worker only; trusted template, no conversation messages.
    Transport *transport_ = nullptr; // Stable boot-lifetime body-store access.
    std::array<char, tx_bytes + 1024> request_body_{}; // One worker, bounded JSON/multipart prefix.
    QueueHandle_t free_ = nullptr, waiting_ = nullptr, done_ = nullptr, previews_ = nullptr;
    ModelPreview preview_snapshot_{}; // Worker-owned, latest snapshot queue never blocks.
    std::array<Job, slots> jobs_{};
    std::atomic<uint32_t> generation_{1};
    std::atomic<uint32_t> outstanding_{0}; // Accepted jobs until worker I/O/borrows have ended.
    std::atomic<uint64_t> speech_cancel_{0};
    bool started_ = false; // Detached pthread has boot lifetime; settings remain frozen.
};
} // namespace wifichat::wifi
