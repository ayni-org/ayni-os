#include "api.h"
#include "../main/harness.h"
#include "../main/speaker.h"
#include "model_codec.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <pthread.h>
#include <string>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr size_t json_limit = 32768, wav_overhead_limit = 16384;
constexpr int64_t request_us = 70000000;
constexpr size_t worker_stack = 12288, network_chunk = 2048;
constexpr int io_slice_ms = 5000, read_poll_ms = 1000;
constexpr size_t upload_frames = 4;
void erase_free(uint8_t *data, size_t bytes) {
    if (!data) {
        return;
    }
    volatile uint8_t *p = data;
    for (size_t at = 0; at < bytes; ++at) {
        p[at] = 0;
    }
    heap_caps_free(data);
}
struct ModelBuffer {
    uint8_t *data = nullptr;
    size_t size = 0;
    explicit ModelBuffer(size_t bytes) : size(bytes) {
        data = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    ~ModelBuffer() { reset(); }
    void reset() {
        erase_free(data, size);
        data = nullptr;
        size = 0;
    }
};
bool endpoint(const char *root, const char *path, char *out, size_t capacity) {
    const auto normalized = normalized_api_root(root);
    const int written =
        std::snprintf(out, capacity, "%.*s%s", int(normalized.size()), normalized.data(), path);
    return written > 0 && size_t(written) < capacity;
}
} // namespace
bool pcm_wav(const uint8_t *raw, size_t bytes, const uint8_t *&pcm, size_t &pcm_bytes) {
    if (!raw || bytes < 44 || std::memcmp(raw, "RIFF", 4) || std::memcmp(raw + 8, "WAVE", 4) ||
        get32(raw + 4) != bytes - 8) {
        return false;
    }
    bool format = false;
    const uint8_t *samples = nullptr;
    size_t size = 0;
    for (size_t at = 12; at < bytes;) {
        if (bytes - at < 8) {
            return false;
        }
        const auto length = get32(raw + at + 4);
        const auto *tag = raw + at;
        at += 8;
        if (length > bytes - at) {
            return false;
        }
        if (!std::memcmp(tag, "fmt ", 4)) {
            if (format || length != 16 || raw[at] != 1 || raw[at + 1] || raw[at + 2] != 1 ||
                raw[at + 3] || get32(raw + at + 4) != Speaker::pcm_rate ||
                get32(raw + at + 8) != Speaker::pcm_rate * 2 || raw[at + 12] != 2 || raw[at + 13] ||
                raw[at + 14] != 16 || raw[at + 15]) {
                return false;
            }
            format = true;
        } else if (!std::memcmp(tag, "data", 4)) {
            if (samples) {
                return false;
            }
            samples = raw + at;
            size = length;
        }
        at += length;
        if (length & 1U) {
            if (at == bytes) {
                return false;
            }
            ++at;
        }
    }
    if (!format || !samples || !size || size % 2 || size > Speaker::maximum_pcm_bytes) {
        return false;
    }
    pcm = samples;
    pcm_bytes = size;
    return true;
}
bool Api::initialize(const Settings &settings, const Transport &transport) {
    transport_ = &transport;
    if (started_ || free_ || waiting_ || done_) {
        return false;
    }
    settings_ = settings;
    free_ = xQueueCreate(slots, sizeof(uint32_t));
    waiting_ = xQueueCreate(slots, sizeof(uint32_t));
    done_ = xQueueCreate(slots, sizeof(uint32_t));
    if (free_ && waiting_ && done_) {
        for (uint32_t i = 0; i < slots; ++i) {
            xQueueSend(free_, &i, 0);
        }
        // FileBodies checks std::thread identity on cold reads. IDF requires a
        // pthread context even when this worker performs no filesystem I/O.
        auto config = esp_pthread_get_default_config();
        config.stack_size = worker_stack;
        config.prio = 2;
        config.thread_name = "wifi-api";
        config.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        pthread_t thread{};
        if (esp_pthread_set_cfg(&config) == ESP_OK &&
            pthread_create(&thread, nullptr, work, this) == 0) {
            started_ = true;
            const auto status = pthread_detach(thread);
            if (status) {
                std::printf("wifi api detach status=%d; worker retained for boot lifetime\n",
                            status);
            }
            return true; // A created worker owns the queues even if detach was refused.
        }
    }
    if (free_) {
        vQueueDelete(free_);
    }
    if (waiting_) {
        vQueueDelete(waiting_);
    }
    if (done_) {
        vQueueDelete(done_);
    }
    free_ = waiting_ = done_ = nullptr;
    settings_.model_key.fill(0);
    settings_.password.fill(0);
    return false;
}
bool Api::current(const Job &job) const {
    return job.generation == generation_.load(std::memory_order_acquire) &&
           !(job.request == Request::speech &&
             job.input.id == speech_cancel_.load(std::memory_order_acquire));
}
bool Api::submit(Request kind, uint64_t id, const void *data, size_t bytes, uint8_t *audio,
                 size_t audio_bytes) {
    if (bytes > tx_bytes || (bytes && !data)) {
        return false;
    }
    uint32_t slot = 0;
    if (xQueueReceive(free_, &slot, 0) != pdTRUE) {
        return false;
    }
    auto &job = jobs_[slot];
    job.request = kind;
    job.input.id = id;
    job.input.size = bytes;
    job.output.id = id;
    job.ok = false;
    job.error.fill(0);
    job.generation = generation_.load(std::memory_order_acquire);
    job.deadline = esp_timer_get_time() + request_us;
    job.audio = audio;
    job.audio_bytes = audio_bytes;
    if (bytes) {
        std::memcpy(job.input.body.data(), data, bytes);
    }
    if (xQueueSend(waiting_, &slot, 0) == pdTRUE) {
        return true;
    }
    job.audio = nullptr;
    job.audio_bytes = 0;
    xQueueSend(free_, &slot, 0);
    return false;
}
Api::Job *Api::result() {
    uint32_t slot = 0;
    return xQueueReceive(done_, &slot, 0) == pdTRUE ? &jobs_[slot] : nullptr;
}
void Api::release(Job *job) {
    const auto index = uint32_t(job - jobs_.data());
    erase_free(job->audio, job->audio_bytes);
    job->audio = nullptr;
    job->audio_bytes = 0;
    erase_free(job->response, job->response_bytes);
    job->response = nullptr;
    job->response_bytes = 0;
    std::fill(job->input.body.begin(), job->input.body.end(), 0);
    std::fill(job->output.body.begin(), job->output.body.end(), 0);
    xQueueSend(free_, &index, 0);
}
void *Api::work(void *context) {
    auto &self = *static_cast<Api *>(context);
    for (;;) {
        uint32_t index = 0;
        if (xQueueReceive(self.waiting_, &index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        auto &job = self.jobs_[index];
        std::printf("wifi api phase=begin request=%u id=%llu\n", unsigned(job.request),
                    static_cast<unsigned long long>(job.input.id));
        if (self.current(job)) {
            if (esp_timer_get_time() < job.deadline) {
                self.perform(job);
            } else {
                std::snprintf(job.error.data(), job.error.size(), "请求排队超时，未发出");
            }
        }
        // Request metadata only; never print transcripts, generated audio or keys.
        std::printf(
            "wifi api request=%u ok=%u current=%u elapsed_ms=%lld error=%s\n",
            unsigned(job.request), unsigned(job.ok), unsigned(self.current(job)),
            static_cast<long long>((esp_timer_get_time() - job.deadline + request_us) / 1000),
            job.error.data());
        std::fill(self.request_body_.begin(), self.request_body_.end(), 0);
        erase_free(job.audio, job.audio_bytes);
        job.audio = nullptr;
        job.audio_bytes = 0;
        // Every queued slot reserves one result slot, so this cannot block on UI work.
        xQueueSend(self.done_, &index, portMAX_DELAY);
    }
}
bool Api::http(Job &job, const char *url, const char *content_type, const void *prefix,
               size_t prefix_bytes, const void *body, size_t body_bytes, const void *suffix,
               size_t suffix_bytes, size_t response_limit, CaptureReader *capture) {
    if (std::strncmp(url, "https://", 8) == 0 && std::time(nullptr) < 1700000000) {
        std::snprintf(job.error.data(), job.error.size(), "等待网络校时后重试");
        return false;
    }
    const auto deadline = job.deadline;
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = int(std::max<int64_t>(
        1, std::min<int64_t>(io_slice_ms, (deadline - esp_timer_get_time() + 999) / 1000)));
    config.buffer_size = network_chunk;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = 0;
    auto client = esp_http_client_init(&config);
    if (!client) {
        std::snprintf(job.error.data(), job.error.size(), "API 客户端初始化失败");
        return false;
    }
    const bool get = job.request == Request::health;
    esp_http_client_set_method(client, get ? HTTP_METHOD_GET : HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    std::array<char, 208> authorization{};
    if (job.request == Request::model) {
        std::snprintf(authorization.data(), authorization.size(), "Bearer %s",
                      settings_.model_key.data());
        esp_http_client_set_header(client, "Authorization", authorization.data());
    }
    const auto expected_upload = prefix_bytes + body_bytes + suffix_bytes;
    int transport_code = esp_http_client_open(client, expected_upload);
    bool ok = transport_code == ESP_OK;
    const char *stage = "连接";
    size_t uploaded = 0;
    if (ok) {
        const int descriptor = esp_http_client_get_socket(client), enabled = 1;
        if (descriptor < 0 ||
            setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) {
            transport_code = errno;
            stage = "TCP 配置";
            ok = false;
        }
    }
    const auto alive = [&] { return current(job) && esp_timer_get_time() < deadline; };
    const auto write = [&](const void *data, size_t size) {
        size_t sent = 0;
        while (sent < size && alive()) {
            const auto count = std::min(network_chunk, size - sent);
            // Uploads can backpressure longer than a read poll. Bound each wait
            // by both the I/O slice and the original job deadline, without replay.
            const auto remaining_ms = (deadline - esp_timer_get_time() + 999) / 1000;
            esp_http_client_set_timeout_ms(
                client, int(std::max<int64_t>(1, std::min<int64_t>(io_slice_ms, remaining_ms))));
            const int wrote =
                esp_http_client_write(client, static_cast<const char *>(data) + sent, count);
            if (wrote <= 0) {
                transport_code = wrote;
                return false; // No replay after an uncertain upload.
            }
            sent += size_t(wrote);
            uploaded += size_t(wrote);
        }
        return sent == size;
    };
    if (ok) {
        stage = "上传";
        ok = write(prefix, prefix_bytes);
    }
    if (ok && capture) {
        int16_t samples[capture::frame_samples * upload_frames]{};
        size_t count = 0, total = 0, buffered = 0;
        while (ok && alive() &&
               capture->next(samples + buffered, std::size(samples) - buffered, count)) {
            buffered += count;
            if (buffered == std::size(samples)) {
                ok = write(samples, buffered * sizeof(int16_t));
                total += buffered * sizeof(int16_t);
                buffered = 0;
            }
        }
        if (ok && buffered) {
            ok = write(samples, buffered * sizeof(int16_t));
            total += buffered * sizeof(int16_t);
        }
        std::fill(std::begin(samples), std::end(samples), 0);
        if (ok && (!capture->complete() || total != body_bytes)) {
            std::snprintf(job.error.data(), job.error.size(), "录音解码不完整，未发送完整请求");
            ok = false;
        }
    } else if (ok) {
        ok = write(body, body_bytes);
    }
    if (ok) {
        ok = write(suffix, suffix_bytes);
    }
    esp_http_client_set_timeout_ms(client, read_poll_ms);
    int64_t length = -1;
    if (ok) {
        stage = "响应头";
    }
    while (ok && alive()) {
        length = esp_http_client_fetch_headers(client);
        if (length != -ESP_ERR_HTTP_EAGAIN) {
            break;
        }
    }
    if (ok && length < 0) {
        transport_code = int(length);
    }
    const int status = esp_http_client_get_status_code(client);
    ok = ok && alive() && length >= 0 && status == 200 && length <= int64_t(response_limit);
    if (!ok && status >= 100 && status <= 599 && status != 200) {
        std::snprintf(job.error.data(), job.error.size(), "API HTTP %d；不会自动重试", status);
    }
    // Complete-WAV serving supplies Content-Length. Allocate its actual length,
    // rather than reserving 30 seconds for even a one-second response.
    const size_t capacity =
        length > 0 && length <= int64_t(response_limit) ? size_t(length) : response_limit;
    if (ok) {
        stage = "响应体";
        job.response = static_cast<uint8_t *>(
            heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!job.response) {
            std::snprintf(job.error.data(), job.error.size(), "API 响应内存不足");
            ok = false;
        }
    }
    while (ok && alive()) {
        // fetch_headers may already have parsed and cached the whole body.
        // Parser completion is not proof that the application drained that cache.
        if (length > 0 && job.response_bytes == size_t(length)) {
            break;
        }
        if (job.response_bytes == capacity) {
            char excess = 0;
            const int count = esp_http_client_read(client, &excess, 1);
            if (count == -ESP_ERR_HTTP_EAGAIN) {
                continue;
            }
            ok = count == 0 && esp_http_client_is_complete_data_received(client);
            break;
        }
        const int got = esp_http_client_read(
            client, reinterpret_cast<char *>(job.response + job.response_bytes),
            std::min(network_chunk, capacity - job.response_bytes));
        if (got == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (got < 0) {
            transport_code = got;
            ok = false;
            break;
        }
        if (!got) {
            ok = esp_http_client_is_complete_data_received(client);
            break;
        }
        job.response_bytes += size_t(got);
    }
    ok = ok && alive() && esp_http_client_is_complete_data_received(client);
    if (job.response) {
        job.response[job.response_bytes] = 0;
    }
    const auto socket_error = esp_http_client_get_errno(client);
    if (!ok) {
        std::printf("wifi http stage=%s status=%d length=%lld code=%d errno=%d upload=%zu/%zu "
                    "response=%zu internal_free=%zu internal_min=%zu\n",
                    stage, status, static_cast<long long>(length), transport_code, socket_error,
                    uploaded, expected_upload, job.response_bytes,
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    std::fill(authorization.begin(), authorization.end(), 0);
    if (!ok && !*job.error.data()) {
        if (!current(job)) {
            std::snprintf(job.error.data(), job.error.size(), "请求已取消；未重试");
        } else if (esp_timer_get_time() >= deadline) {
            std::snprintf(job.error.data(), job.error.size(), "API 请求总期限已到；未重试");
        } else {
            std::snprintf(job.error.data(), job.error.size(), "%s失败（%d/%d）；未重试", stage,
                          transport_code, socket_error);
        }
    }
    return ok;
}
void Api::perform(Job &job) {
    std::array<char, 256> url{};
    const char *root =
        job.request == Request::model ? settings_.model_url.data() : settings_.speech_url.data();
    const char *path = job.request == Request::model       ? "/chat/completions"
                       : job.request == Request::speech    ? "/v1/audio/speech"
                       : job.request == Request::recognize ? "/v1/audio/transcriptions"
                                                           : "/readyz";
    if (!endpoint(root, path, url.data(), url.size())) {
        std::snprintf(job.error.data(), job.error.size(), "API 地址超限");
        return;
    }
    switch (job.request) {
    case Request::model:
        model(job, url.data());
        break;
    case Request::speech:
        speech(job, url.data());
        break;
    case Request::recognize:
        recognize(job, url.data());
        break;
    case Request::health:
        job.ok = http(job, url.data(), "application/json", nullptr, 0, nullptr, 0, nullptr, 0,
                      json_limit);
        break;
    }
}
void Api::model(Job &job, const char *url) {
    if (job.input.size != model_request_bytes || !transport_ || !transport_->bodies() ||
        get32(job.input.body.data() + body_reference_bytes) > 1) {
        std::snprintf(job.error.data(), job.error.size(), "模型正文引用无效");
        return;
    }
    const auto reference = get_body_reference(job.input.body.data());
    const bool tools_allowed = get32(job.input.body.data() + body_reference_bytes) == 1;
    if (!reference.id || !reference.size || reference.size > harness::model_body_bytes ||
        reference.reserved) {
        std::snprintf(job.error.data(), job.error.size(), "模型正文超限");
        return;
    }
    ModelBuffer framed(reference.size);
    if (!framed.data) {
        std::snprintf(job.error.data(), job.error.size(), "上下文内存不足，未发出模型请求");
        return;
    }
    const auto *store = transport_->bodies();
    auto status = AYNI_NOT_READY;
    while (current(job) && esp_timer_get_time() < job.deadline) {
        status = store->read(store->context, &reference, framed.data, reference.size);
        if (status != AYNI_NOT_READY) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Owner still polls the SDK file store.
    }
    if (status != AYNI_OK || !current(job) ||
        ayni_body_verify(&reference, {framed.data, reference.size}) != AYNI_OK) {
        std::snprintf(job.error.data(), job.error.size(), "模型正文不可读、超时或已取消；未重试");
        return;
    }
    const ayni_string_v1 context{reinterpret_cast<const char *>(framed.data), reference.size};
    const auto encode = [&](ayni::json::Writer &out) {
        out.raw("{\"model\":");
        out.string(std::string_view(settings_.model.data()));
        out.raw(",\"stream\":false,\"max_tokens\":");
        out.unsigned_number(harness::max_output_tokens);
        if (tools_allowed) {
            out.raw(",\"parallel_tool_calls\":false");
        }
        if (std::strstr(settings_.model_url.data(), "https://api.deepseek.com") ==
            settings_.model_url.data()) {
            out.raw(",\"thinking\":{\"type\":\"disabled\"}");
        }
        out.raw(",");
        const auto status =
            tools_allowed ? ayni::model::write_context(out, context, ayni::model::Mode::assistant)
                          : ayni::model::write_context(out, context, ayni::model::Mode::text);
        out.raw("}");
        return status;
    };
    auto measured = ayni::json::Writer::measuring();
    size_t size = 0;
    if (encode(measured) != AYNI_OK || !measured.finish(size) || size > harness::model_body_bytes) {
        std::snprintf(job.error.data(), job.error.size(), "模型上下文编码超限");
        return;
    }
    ModelBuffer json(size);
    if (!json.data) {
        std::snprintf(job.error.data(), job.error.size(), "上下文发送内存不足；未请求模型");
        return;
    }
    ayni::json::Writer out(reinterpret_cast<char *>(json.data), size);
    if (encode(out) != AYNI_OK || !out.finish(size)) {
        std::snprintf(job.error.data(), job.error.size(), "模型上下文编码失败");
        return;
    }
    framed.reset(); // Only the projected JSON remains during the HTTP request.
    if (http(job, url, "application/json", nullptr, 0, json.data, size, nullptr, 0, json_limit)) {
        job.ok = decode_model({reinterpret_cast<const char *>(job.response), job.response_bytes},
                              tools_allowed, job.output);
        if (!job.ok) {
            std::snprintf(job.error.data(), job.error.size(),
                          "模型必须返回完整文本，工具调用或超限输出已拒绝");
        }
    }
}
void Api::speech(Job &job, const char *url) {
    const std::string_view input(reinterpret_cast<const char *>(job.input.body.data()),
                                 job.input.size);
    ayni::json::Writer out(request_body_.data(), request_body_.size());
    out.raw("{\"model\":\"qwen3-tts\",\"input\":");
    out.string(input);
    out.raw(",\"response_format\":\"wav\",\"sample_rate\":16000}");
    size_t size = 0;
    if (!out.finish(size)) {
        std::snprintf(job.error.data(), job.error.size(), "朗读请求超限");
        return;
    }
    const uint8_t *pcm = nullptr;
    size_t bytes = 0;
    job.ok = http(job, url, "application/json", nullptr, 0, request_body_.data(), size, nullptr, 0,
                  Speaker::maximum_pcm_bytes + wav_overhead_limit) &&
             pcm_wav(job.response, job.response_bytes, pcm, bytes);
    if (!job.ok && !*job.error.data()) {
        std::snprintf(job.error.data(), job.error.size(), "语音响应不是完整的 16 kHz PCM WAV");
    }
}
void Api::recognize(Job &job, const char *url) {
    CaptureReader capture;
    if (!capture.initialize(job.audio, job.audio_bytes)) {
        std::snprintf(job.error.data(), job.error.size(), "录音格式无效或超限");
        return;
    }
    constexpr std::string_view prefix =
        "--ayni-capture-1\r\nContent-Disposition: form-data; "
        "name=\"model\"\r\n\r\nqwen3-asr\r\n--ayni-capture-1\r\nContent-Disposition: form-data; "
        "name=\"response_format\"\r\n\r\njson\r\n--ayni-capture-1\r\nContent-Disposition: "
        "form-data; name=\"file\"; filename=\"capture.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    constexpr std::string_view suffix = "\r\n--ayni-capture-1--\r\n";
    constexpr size_t wav_header_bytes = 44;
    std::memcpy(request_body_.data(), prefix.data(), prefix.size());
    auto *wav = reinterpret_cast<uint8_t *>(request_body_.data() + prefix.size());
    std::memset(wav, 0, wav_header_bytes);
    std::memcpy(wav, "RIFF", 4);
    put32(wav + 4, capture.pcm_bytes() + 36);
    std::memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16);
    wav[20] = 1;
    wav[22] = 1;
    put32(wav + 24, Speaker::pcm_rate);
    put32(wav + 28, Speaker::pcm_rate * 2);
    wav[32] = 2;
    wav[34] = 16;
    std::memcpy(wav + 36, "data", 4);
    put32(wav + 40, capture.pcm_bytes());
    if (http(job, url, "multipart/form-data; boundary=ayni-capture-1", request_body_.data(),
             prefix.size() + wav_header_bytes, nullptr, capture.pcm_bytes(), suffix.data(),
             suffix.size(), json_limit, &capture)) {
        auto *doc =
            yyjson_read(reinterpret_cast<const char *>(job.response), job.response_bytes, 0);
        if (doc) {
            auto *value = yyjson_obj_get(yyjson_doc_get_root(doc), "text");
            const auto size = yyjson_get_len(value);
            if (yyjson_is_str(value) && size && size <= input_bytes &&
                !std::memchr(yyjson_get_str(value), 0, size)) {
                job.output.kind = Kind::user;
                job.output.size = size;
                std::memcpy(job.output.body.data(), yyjson_get_str(value), size);
                job.ok = true;
            }
            yyjson_doc_free(doc);
        }
    }
    if (!job.ok && !*job.error.data()) {
        std::snprintf(job.error.data(), job.error.size(), "未识别到文字或转写超限");
    }
}
} // namespace wifichat::wifi
