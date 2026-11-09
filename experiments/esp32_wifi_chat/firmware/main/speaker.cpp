#include "speaker.h"
#include "transport.h"
#include "wake.h"
#include <algorithm>
#include <array>
#include <ayni/authority.h>
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/values.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <new>
namespace wifichat {
namespace {
constexpr uint32_t version = 1, output_rate = 48000, stereo_channels = 2,
                   repeat_samples = output_rate / Speaker::pcm_rate;
constexpr uint32_t block_samples = 256, dma_buffers = 4, dma_frames = 256,
                   audio_stack_bytes = 12288;
constexpr uint32_t minimum_hz = 100, maximum_hz = 4000, minimum_tone_ms = 50,
                   maximum_tone_ms = 3000;
constexpr uint32_t synthesis_timeout_ms = SpeechProfile::timeout_ms,
                   legacy_tone_timeout_ms = 120000, write_timeout_ms = 200,
                   maximum_text_bytes = 1024;
constexpr float tau = 6.2831853071795864769F;
constexpr gpio_num_t speaker_bclk = GPIO_NUM_4, speaker_ws = GPIO_NUM_5, speaker_data = GPIO_NUM_6;
constexpr uint32_t result_limit = Speaker::result_bytes;
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && !std::memcmp(a.data, b, a.size);
}
void number(ayni_schema_node_v1 &n, uint32_t field, const char *name, const char *unit,
            uint32_t end, uint32_t low, uint32_t high) {
    n.kind = AYNI_VALUE_U64;
    n.field_id = field;
    n.subtree_end = end;
    n.flags = AYNI_SCHEMA_REQUIRED;
    n.name = text(name);
    n.unit = text(unit);
    n.min_u64 = low;
    n.max_u64 = high;
}
} // namespace
esp_err_t Speaker::park_outputs() {
    // BCLK must stop first; the MAX98357A then enters standby without an SD pin.
    // gpio_config disconnects the old peripheral matrix output after I2S deletion.
    for (const auto pin : {speaker_bclk, speaker_ws, speaker_data}) {
        auto rc = gpio_set_level(pin, 0);
        if (rc != ESP_OK) {
            return rc;
        }
        gpio_config_t config{};
        config.pin_bit_mask = UINT64_C(1) << pin;
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_down_en = GPIO_PULLDOWN_ENABLE;
        rc = gpio_config(&config);
        if (rc != ESP_OK) {
            return rc;
        }
    }
    return ESP_OK;
}
struct Speaker::Worker {
    Speaker &owner;
    StaticTask_t tcb{};
    std::array<StackType_t, audio_stack_bytes / sizeof(StackType_t)> stack{};
    TaskHandle_t task = nullptr;
    QueueHandle_t jobs = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    std::array<int16_t, block_samples * repeat_samples * stereo_channels> output{};
    std::array<int16_t, block_samples> input{};
    std::atomic<bool> done{false};
    esp_err_t error = ESP_OK;
    uint32_t accepted_samples = 0;
    bool drained = false;
    explicit Worker(Speaker &s) : owner(s) {}
    esp_err_t play(Job);
    esp_err_t write_samples(Job, i2s_chan_handle_t);
};
bool Speaker::initialize() {
    if (worker_) {
        return worker_->task != nullptr;
    }
    void *memory = heap_caps_malloc(sizeof(Worker), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        heap_caps_free(memory);
        heap_caps_free(pcm_.detach());
        return false;
    }
    worker_ = new (memory) Worker(*this);
    worker_->jobs = xQueueCreate(1, sizeof(Job));
    worker_->stopped = xSemaphoreCreateBinary();
    if (worker_->jobs && worker_->stopped) {
        worker_->task = xTaskCreateStatic(run, "speaker-i2s", audio_stack_bytes, worker_, 3,
                                          worker_->stack.data(), &worker_->tcb);
    }
    if (worker_->task) {
        return true;
    }
    if (worker_->jobs) {
        vQueueDelete(worker_->jobs);
    }
    if (worker_->stopped) {
        vSemaphoreDelete(worker_->stopped);
    }
    worker_->~Worker();
    heap_caps_free(worker_);
    worker_ = nullptr;
    heap_caps_free(pcm_.detach());
    return false;
}
void Speaker::status(const char *s) {
    transport_.send(Kind::status, pending_ ? pending_->call : 0, s, std::strlen(s));
}
ayni_status_t Speaker::configure(ayni_kernel_t *kernel) {
    if (!initialize()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    volume_default_node_.kind = AYNI_VALUE_U64;
    volume_default_node_.subtree_end = 1;
    volume_default_node_.value.unsigned_integer = default_volume;
    volume_default_ = {&volume_default_node_, 1};
    tone_input_[0].kind = AYNI_VALUE_OBJECT;
    tone_input_[0].subtree_end = 4;
    number(tone_input_[1], 1, "frequency_hz", "hz", 2, minimum_hz, maximum_hz);
    number(tone_input_[2], 2, "duration_ms", "ms", 3, minimum_tone_ms, maximum_tone_ms);
    number(tone_input_[3], 3, "volume_percent", "percent", 4, 1, 100);
    tone_input_[3].flags = 0;
    tone_input_[3].default_value = &volume_default_;
    speech_input_[0].kind = AYNI_VALUE_OBJECT;
    speech_input_[0].subtree_end = 3;
    auto &body = speech_input_[1];
    body.kind = AYNI_VALUE_TEXT;
    body.field_id = 1;
    body.subtree_end = 2;
    body.flags = AYNI_SCHEMA_REQUIRED;
    body.name = text("text");
    body.min_count = 1;
    body.max_count = maximum_text_bytes;
    number(speech_input_[2], 2, "volume_percent", "percent", 3, 1, 100);
    speech_input_[2].flags = 0;
    speech_input_[2].default_value = &volume_default_;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = result_limit;
    businesses_[0] = {{tone_input_, 4}, {&result_, 1}, 0, 0, nullptr};
    businesses_[1] = {{speech_input_, 3}, {&result_, 1}, 0, 0, nullptr};
    resource_ = {text("speaker.slot"), 1, 0};
    const char *capabilities[] = {"speaker.tone", "speaker.speech"};
    const char *bindings[] = {"speaker.play_tone", "speaker.speak_text"};
    for (uint32_t at = 0; at < 2; ++at) {
        const auto capability_version = at == 0 ? version : SpeechProfile::capability_version;
        offers_[at] = {sizeof(ayni_call_capability_v1),
                       capability_version,
                       text(capabilities[at]),
                       {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                        AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                        at == 0 ? legacy_tone_timeout_ms : synthesis_timeout_ms,
                        AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, input_bytes(), result_limit,
                        1, &resource_, nullptr},
                       &businesses_[at]};
        const ayni_capability_desc_v1 cap{
            sizeof(cap), capability_version, text(capabilities[at]), 0, 0, nullptr};
        auto rc = ayni_register_capability(kernel, &cap);
        if (rc == AYNI_OK) {
            rc = ayni_register_call_capability(kernel, &offers_[at]);
        }
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 text("speaker.provider"),
                 text("speaker.i2s"),
                 {},
                 this,
                 2,
                 0,
                 offers_,
                 submit,
                 cancel,
                 nullptr,
                 nullptr};
    auto rc = ayni_register_provider(kernel, &provider_);
    const ayni_resource_desc_v1 slot{
        sizeof(slot), AYNI_RESOURCE_EXCLUSIVE, text("speaker.slot"), 1, 0, 0, 0, nullptr, {}};
    if (rc == AYNI_OK) {
        rc = ayni_register_resource(kernel, &slot);
    }
    if (rc == AYNI_OK) {
        rc = ayni_set_resource_health(kernel, slot.resource_id, AYNI_RESOURCE_ONLINE);
    }
    for (uint32_t at = 0; rc == AYNI_OK && at < 2; ++at) {
        const auto capability_version = at == 0 ? version : SpeechProfile::capability_version;
        const ayni_binding_v1 binding{sizeof(binding),
                                      capability_version,
                                      text(bindings[at]),
                                      text(capabilities[at]),
                                      capability_version,
                                      provider_.version,
                                      provider_.provider_id,
                                      provider_.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        rc = ayni_register_binding(kernel, &binding);
        const ayni_tool_usage_v1 usage{
            sizeof(usage),
            binding.version,
            binding.binding_id,
            capability_version,
            text(capabilities[at]),
            text(at == 0 ? "Play a short local speaker notification tone when requested."
                         : "Speak text through the ESP32 speaker using the configured speech API."),
            text(at == 0 ? "frequency_hz 100..4000; duration_ms 50..3000; optional volume_percent "
                           "1..100, default 80."
                         : "Required text 1..1024 UTF-8 bytes; optional volume_percent 1..100, "
                           "default 80. Native PCM streams while generating, up to 300 seconds."),
            text("Success requires a complete audio stream, all PCM drained through I2S and the "
                 "channel stopped. It does not "
                 "prove a human heard sound. Unknown output must never be retried automatically."),
            text(at == 0 ? "{\"frequency_hz\":660,\"duration_ms\":300}"
                         : "{\"text\":\"你好，我是 Ayni。\"}"),
            0,
            0,
            nullptr};
        if (rc == AYNI_OK) {
            rc = ayni_register_capability_usage(
                kernel,
                text(at == 0 ? "Play a bounded speaker tone"
                             : "Speak Chinese text on the ESP32 speaker"),
                &usage);
        }
    }
    return rc;
}
// The speech input at its text limit, measured from its shape; the tone input is smaller.
uint32_t Speaker::input_bytes() {
    ayni::ValueBuilder<3> shape;
    shape.object();
    shape.text(1, {});
    shape.unsigned_integer(2, 0);
    shape.end();
    ayni_value_v1 value{};
    return shape.finish(value) == AYNI_OK ? ayni::encoded_bytes(value, maximum_text_bytes) : 0U;
}
bool Speaker::enqueue(Job job) {
    worker_->done.store(false, std::memory_order_release);
    if (xQueueSend(worker_->jobs, &job, 0) != pdTRUE) {
        return false;
    }
    queued_ = true;
    return true;
}
ayni_submit_result_t Speaker::submit(void *ctx, const ayni_provider_request_v1 *request,
                                     ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Speaker *>(ctx);
    if (!self.accepting_ || !request || !completion || self.pending_ || !self.worker_) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader value;
    if (value.read(request->value) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const bool speech = same(request->capability_id, "speaker.speech");
    const auto *volume = value.field(speech ? 2 : 3);
    if (!volume || volume->kind != AYNI_VALUE_U64) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.cancel_.store(false, std::memory_order_release);
    self.queued_ = false;
    self.pending_ = Pending{speech ? Operation::speech : Operation::tone,
                            request->logical_call_id,
                            request->deadline_us,
                            uint32_t(volume->value.unsigned_integer),
                            request->attempt,
                            completion};
    if (!speech) {
        const auto *frequency = value.field(1), *duration = value.field(2);
        if (!frequency || !duration || frequency->kind != AYNI_VALUE_U64 ||
            duration->kind != AYNI_VALUE_U64 ||
            !self.enqueue({Operation::tone, uint32_t(frequency->value.unsigned_integer),
                           uint32_t(duration->value.unsigned_integer) * pcm_rate / 1000,
                           uint32_t(volume->value.unsigned_integer)})) {
            self.pending_.reset();
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
    } else {
        ayni_string_v1 body{};
        if (!ayni::ValueReader::text(value.field(1), body)) {
            self.pending_.reset();
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        if (self.begin_speech(request->logical_call_id, {body.data, body.size}) != AYNI_OK) {
            self.pending_.reset();
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
    }
    self.status(speech ? "speaker speech dispatched" : "speaker tone dispatched");
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t Speaker::cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Speaker *>(ctx);
    if (!self.pending_ || self.pending_->attempt.boot_epoch != attempt.boot_epoch ||
        self.pending_->attempt.request_id != attempt.request_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (self.queued_ && self.worker_->done.load(std::memory_order_acquire)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED; // Preserve an already known result.
    }
    self.request_cancel();
    wake_owner(); // An unqueued transfer fails on the owner's next advance.
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_status_t Speaker::begin_speech(uint64_t id, std::string_view text) {
    auto *storage = static_cast<uint8_t *>(
        heap_caps_calloc(1, SpeechProfile::buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    portENTER_CRITICAL(&mutex_);
    transfer_ = Transfer::begin;
    transfer_call_ = id;
    pcm_.attach(storage);
    portEXIT_CRITICAL(&mutex_);
    if (transport_.send(Kind::speech_request, id, text.data(), text.size())) {
        return AYNI_OK;
    }
    portENTER_CRITICAL(&mutex_);
    transfer_ = Transfer::none;
    portEXIT_CRITICAL(&mutex_);
    heap_caps_free(pcm_.detach());
    return AYNI_BUSY;
}
void Speaker::request_cancel() {
    cancel_.store(true, std::memory_order_release);
    portENTER_CRITICAL(&mutex_);
    transfer_ = Transfer::failed;
    portEXIT_CRITICAL(&mutex_);
    transport_.send(Kind::audio_cancel, pending_->call, nullptr, 0);
    wake_owner();
}
void Speaker::request_stop() {
    if (pending_ && !(queued_ && worker_->done.load(std::memory_order_acquire))) {
        request_cancel();
    }
}
// Framing transitions share the ring lock with append/read. A successful end
// closes input only after the API worker validated both AYTS and HTTP completion.
bool Speaker::receive(Kind kind, uint64_t call, const uint8_t *data, size_t bytes) {
    bool okay = false;
    portENTER_CRITICAL(&mutex_);
    if (call == transfer_call_ && transfer_ != Transfer::none && transfer_ != Transfer::failed) {
        if (kind == Kind::audio_begin && transfer_ == Transfer::begin && bytes == 8 &&
            get32(data) == speech_rate && get32(data + 4) == maximum_pcm_bytes) {
            transfer_ = Transfer::data;
            okay = true;
        } else if (kind == Kind::audio_end && transfer_ == Transfer::data && bytes == 4 &&
                   pcm_.received() && get32(data) == pcm_.received()) {
            transfer_ = Transfer::ready;
            okay = true;
        } else if (kind == Kind::audio_error && transfer_ != Transfer::ready) {
            transfer_ = Transfer::failed;
        }
    }
    portEXIT_CRITICAL(&mutex_);
    wake_owner();
    return okay;
}
AudioWrite Speaker::append_audio(uint64_t call, uint32_t offset, const uint8_t *data,
                                 size_t bytes) {
    portENTER_CRITICAL(&mutex_);
    const auto result = call == transfer_call_ && transfer_ == Transfer::data && !cancel_.load()
                            ? pcm_.append(offset, data, bytes)
                            : AudioWrite::rejected;
    portEXIT_CRITICAL(&mutex_);
    if (result == AudioWrite::accepted) {
        wake_owner();
    }
    return result;
}
ayni_status_t Speaker::advance() {
    if (!pending_) {
        return AYNI_OK;
    }
    const bool expired = uint64_t(esp_timer_get_time()) >= pending_->deadline;
    const bool already_done = queued_ && worker_->done.load(std::memory_order_acquire);
    if (expired && !already_done && !cancel_.load()) {
        cancel_.store(true, std::memory_order_release);
        transport_.send(Kind::audio_cancel, pending_->call, nullptr, 0);
        portENTER_CRITICAL(&mutex_);
        transfer_ = Transfer::failed;
        portEXIT_CRITICAL(&mutex_);
    }
    Transfer transfer;
    uint32_t available, volume = pending_->volume;
    portENTER_CRITICAL(&mutex_);
    transfer = transfer_;
    available = pcm_.available();
    portEXIT_CRITICAL(&mutex_);
    if (!queued_ && pending_->operation == Operation::speech &&
        (transfer == Transfer::data || transfer == Transfer::ready) &&
        (available >= speech_rate * sizeof(int16_t) / 10 ||
         (transfer == Transfer::ready && available)) &&
        !cancel_.load()) {
        if (!enqueue({Operation::speech, 0, 0, volume})) {
            return AYNI_BUSY;
        }
        status("speaker playback queued");
    }
    const bool no_output = !queued_ && (transfer == Transfer::failed || cancel_.load());
    if (!no_output && (!queued_ || !worker_->done.load(std::memory_order_acquire))) {
        return AYNI_OK;
    }
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = pending_->attempt;
    char facts[result_limit]{};
    if (!no_output && worker_->drained && worker_->error == ESP_OK && !cancel_.load()) {
        result.outcome = AYNI_CALL_COMPLETED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        std::snprintf(facts, sizeof(facts),
                      "{\"played\":true,\"samples\":%lu,\"sample_rate\":%lu,\"volume_percent\":%"
                      "lu,\"confirmed\":\"i2s-drained\"}",
                      static_cast<unsigned long>(worker_->accepted_samples),
                      static_cast<unsigned long>(
                          pending_->operation == Operation::speech ? speech_rate : pcm_rate),
                      static_cast<unsigned long>(volume));
        result.result = text(facts);
    } else if (no_output || !worker_->accepted_samples) {
        result.outcome = cancel_.load() && !expired ? AYNI_CALL_CANCELLED : AYNI_CALL_FAILED;
        result.status = expired ? AYNI_EXPIRED : cancel_.load() ? AYNI_OK : AYNI_NOT_READY;
        result.effect = AYNI_CALL_EFFECT_NONE;
    } else {
        result.outcome = AYNI_CALL_ABANDONED;
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    }
    const auto posted = ayni_provider_post(pending_->completion, &result);
    if (posted == AYNI_OK) {
        status(result.outcome == AYNI_CALL_COMPLETED
                   ? "speaker playback completed"
                   : "speaker playback failed/unknown; no automatic retry");
        pending_.reset();
        queued_ = false;
        portENTER_CRITICAL(&mutex_);
        transfer_ = Transfer::none;
        auto *released = pcm_.detach();
        portEXIT_CRITICAL(&mutex_);
        // No receiver can retain a pointer after the lock; worker completion ends reads.
        if (released) {
            volatile uint8_t *wipe = reinterpret_cast<uint8_t *>(released);
            for (uint32_t at = 0; at < SpeechProfile::buffer_bytes; ++at) {
                wipe[at] = 0;
            }
        }
        heap_caps_free(released);
    }
    return posted == AYNI_BUSY ? AYNI_OK : posted;
}
esp_err_t Speaker::Worker::play(Job job) {
    accepted_samples = 0;
    drained = false;
    if (owner.cancel_.load()) {
        return ESP_OK;
    }
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = dma_buffers;
    channel.dma_frame_num = dma_frames;
    channel.auto_clear = true;
    auto rc = i2s_new_channel(&channel, &tx, nullptr);
    if (rc != ESP_OK) {
        return rc;
    }
    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(output_rate);
    config.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = speaker_bclk;
    config.gpio_cfg.ws = speaker_ws;
    config.gpio_cfg.dout = speaker_data;
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    rc = i2s_channel_init_std_mode(tx, &config);
    bool enabled = false;
    if (rc == ESP_OK) {
        rc = i2s_channel_enable(tx);
        enabled = rc == ESP_OK;
    }
    if (rc == ESP_OK) {
        rc = write_samples(job, tx);
    }
    // The driver leases each finished DMA buffer through its TX queue. Writing
    // more than one complete ring of zeros proves the preceding last PCM buffer
    // has been consumed, so disabling below can truncate only padding silence.
    if (rc == ESP_OK && !owner.cancel_.load()) {
        output.fill(0);
        size_t remaining = (dma_buffers + 1) * dma_frames * stereo_channels * sizeof(int16_t);
        while (rc == ESP_OK && remaining) {
            const size_t bytes = std::min(remaining, output.size() * sizeof(int16_t));
            size_t written = 0;
            rc = i2s_channel_write(tx, output.data(), bytes, &written, write_timeout_ms);
            if (rc == ESP_OK && written != bytes) {
                rc = ESP_ERR_TIMEOUT;
            }
            remaining -= written;
        }
        drained = rc == ESP_OK;
    }
    if (enabled) {
        const auto disabled = i2s_channel_disable(tx);
        if (rc == ESP_OK) {
            rc = disabled;
        }
    }
    const auto deleted = i2s_del_channel(tx);
    const auto parked = deleted == ESP_OK ? Speaker::park_outputs() : deleted;
    if (rc == ESP_OK) {
        rc = parked;
    }
    return rc;
}
esp_err_t Speaker::Worker::write_samples(Job job, i2s_chan_handle_t tx) {
    esp_err_t rc = ESP_OK;
    float phase = 0;
    const uint32_t repeat =
        job.operation == Operation::speech ? output_rate / speech_rate : repeat_samples;
    uint32_t at = 0;
    while (rc == ESP_OK && !owner.cancel_.load()) {
        uint32_t count = 0;
        if (job.operation == Operation::speech) {
            portENTER_CRITICAL(&owner.mutex_);
            const auto state = owner.transfer_;
            if (state == Transfer::failed || state == Transfer::none) {
                portEXIT_CRITICAL(&owner.mutex_);
                rc = ESP_FAIL;
                break;
            }
            count = owner.pcm_.read(input.data(), block_samples);
            portEXIT_CRITICAL(&owner.mutex_);
            if (!count) {
                if (state == Transfer::ready) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        } else {
            if (at == job.samples) {
                break;
            }
            count = std::min(block_samples, job.samples - at);
        }
        for (uint32_t n = 0; n < count; ++n) {
            int32_t sample = 0;
            if (job.operation == Operation::speech) {
                sample = input[n];
            } else {
                sample = int32_t(std::sin(phase) * 32767.0F);
                phase += tau * job.frequency / pcm_rate;
                if (phase >= tau) {
                    phase -= tau;
                }
            }
            sample = sample * int32_t(job.volume) / 100;
            for (uint32_t repeated = 0; repeated < repeat * stereo_channels; ++repeated) {
                output[n * repeat * stereo_channels + repeated] = int16_t(sample);
            }
        }
        size_t written = 0;
        const size_t bytes = count * repeat * stereo_channels * sizeof(int16_t);
        rc = i2s_channel_write(tx, output.data(), bytes, &written, write_timeout_ms);
        accepted_samples += written / (repeat * stereo_channels * sizeof(int16_t));
        if (rc == ESP_OK && written != bytes) {
            rc = ESP_ERR_TIMEOUT;
        }
        at += count;
    }
    return rc;
}
void Speaker::run(void *ctx) {
    auto &worker = *static_cast<Worker *>(ctx);
    Job job;
    while (xQueueReceive(worker.jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (job.operation == Operation::quit) {
            break;
        }
        worker.error = worker.play(job);
        worker.input.fill(0);
        worker.output.fill(0);
        worker.done.store(true, std::memory_order_release);
        wake_owner(); // The owner posts the result; Core cannot see the worker.
    }
    xSemaphoreGive(worker.stopped);
    vTaskSuspend(nullptr);
}
uint32_t Speaker::stack_free_bytes() const {
    return worker_ ? uxTaskGetStackHighWaterMark(worker_->task) : final_stack_free_;
}
bool Speaker::close() {
    if (pending_) {
        return false;
    }
    if (!worker_) {
        return true;
    }
    const Job quit{Operation::quit, 0, 0, 0};
    if (xQueueSend(worker_->jobs, &quit, pdMS_TO_TICKS(1000)) != pdTRUE ||
        xSemaphoreTake(worker_->stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    // Same pinned IDF6 legacy-kernel join rule as the existing Host: a suspended
    // state excludes execution on either CPU before static task memory is freed.
    while (eTaskGetState(worker_->task) != eSuspended) {
        vTaskDelay(1);
    }
    final_stack_free_ = uxTaskGetStackHighWaterMark(worker_->task);
    vTaskDelete(worker_->task);
    vQueueDelete(worker_->jobs);
    vSemaphoreDelete(worker_->stopped);
    worker_->~Worker();
    heap_caps_free(worker_);
    worker_ = nullptr;
    return true;
}
} // namespace wifichat
