#include "api.h"
#include "../main/harness.h"
#include "../main/speaker.h"
#include "model_buffer.h"
#include "model_codec.h"
#include "model_delivery.h"
#include "network_time.h"
#include "response_limit.h"
#include "speech_stream.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <ayni/providers/model_protocol.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <string>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr size_t json_limit = 32768, wav_overhead_limit = 16384;
constexpr int64_t request_us = 70000000;
constexpr int64_t request_budget_us(Request request) {
    return request == Request::speech ? int64_t(SpeechProfile::timeout_ms) * 1000 : request_us;
}
constexpr size_t worker_stack = 12288, network_chunk = 2048;
constexpr int io_slice_ms = 5000, read_poll_ms = 1000;
constexpr size_t upload_frames = 4;
static_assert(Speaker::speech_rate == SpeechStream::rate &&
              Speaker::maximum_pcm_bytes == SpeechStream::max_pcm_bytes);
bool endpoint(const char *root, const char *path, char *out, size_t capacity) {
    const auto normalized = normalized_api_root(root);
    const int written =
        std::snprintf(out, capacity, "%.*s%s", int(normalized.size()), normalized.data(), path);
    return written > 0 && size_t(written) < capacity;
}
// Observe classified local/business tool facts, never text, results or argument values.
void trace_tool_status(DiagnosticLog &log, ayni_string_v1 context) {
    ayni_string_v1 instructions{}, messages{};
    ayni_string_v1 definitions{};
    if (ayni::model::split_request(context, messages, definitions) != AYNI_OK ||
        ayni::model::split_instructions(messages, instructions, messages) != AYNI_OK) {
        return;
    }
    ayni::model::Reader reader(messages);
    ayni::model::Message message;
    ayni_string_v1 last{};
    while (reader.next(message)) {
        if (message.role == ayni::model::Role::tool) {
            last = message.text;
        }
    }
    if (!reader.finished() || !last.size || last.size > json_limit) {
        return;
    }
    auto *doc = yyjson_read(last.data, last.size, 0);
    if (!doc) {
        return;
    }
    auto *root = yyjson_doc_get_root(doc);
    auto *status = yyjson_obj_get(yyjson_obj_get(root, "error"), "status");
    if (yyjson_is_int(status)) {
        log.record("wifi tool result_status=%lld\n",
                   static_cast<long long>(yyjson_get_sint(status)));
    } else {
        auto *outcome = yyjson_obj_get(root, "outcome");
        status = yyjson_obj_get(root, "status");
        auto *reason = yyjson_obj_get(root, "reason");
        auto *effect = yyjson_obj_get(root, "effect");
        if (yyjson_is_uint(outcome) && yyjson_is_int(status) && yyjson_is_uint(reason) &&
            yyjson_is_uint(effect)) {
            log.record("wifi tool outcome=%llu result_status=%lld reason=%llu effect=%llu\n",
                       static_cast<unsigned long long>(yyjson_get_uint(outcome)),
                       static_cast<long long>(yyjson_get_sint(status)),
                       static_cast<unsigned long long>(yyjson_get_uint(reason)),
                       static_cast<unsigned long long>(yyjson_get_uint(effect)));
        }
    }
    yyjson_doc_free(doc);
}
} // namespace
bool Api::initialize(const Settings &settings, Transport &transport) {
    transport_ = &transport;
    if (started_ || free_ || waiting_ || done_ || !settings.valid()) {
        return false;
    }
    settings_ = settings;
    free_ = xQueueCreate(slots, sizeof(uint32_t));
    waiting_ = xQueueCreate(slots, sizeof(uint32_t));
    done_ = xQueueCreate(slots, sizeof(uint32_t));
    previews_ = xQueueCreate(1, sizeof(ModelPreview));
    if (free_ && waiting_ && done_ && previews_) {
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
                transport_->diagnostics().record(
                    "wifi api detach status=%d; worker retained for boot lifetime\n", status);
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
    if (previews_) {
        vQueueDelete(previews_);
    }
    free_ = waiting_ = done_ = previews_ = nullptr;
    settings_.model_key.fill(0);
    settings_.password.fill(0);
    return false;
}
bool Api::take_preview(ModelPreview &out) {
    if (previews_ && xQueueReceive(previews_, &out, 0) == pdTRUE &&
        out.generation == transport_->api_generation()) {
        return true;
    }
    std::fill(out.text.begin(), out.text.end(), 0);
    out.size = 0;
    out.id = 0;
    return false;
}
bool Api::current(const Job &job) const {
    return job.generation == transport_->api_generation() &&
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
    job.output.kind = {};
    job.output.size = 0;
    job.outline.clear();
    job.ok = false;
    job.error.fill(0);
    job.generation = transport_->api_generation();
    job.deadline = esp_timer_get_time() + request_budget_us(kind);
    job.http_complete_us = 0;
    job.ready_us = 0;
    job.audio = audio;
    job.audio_bytes = audio_bytes;
    if (bytes) {
        std::memcpy(job.input.body.data(), data, bytes);
    }
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    if (xQueueSend(waiting_, &slot, 0) == pdTRUE) {
        return true;
    }
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
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
    job->outline.clear();
    job->http_complete_us = 0;
    job->ready_us = 0;
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
        const auto internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        self.transport_->diagnostics().record(
            "wifi api phase=begin request=%u id=%llu queue_ms=%lld\n", unsigned(job.request),
            static_cast<unsigned long long>(job.input.id),
            static_cast<long long>(
                (esp_timer_get_time() - job.deadline + request_budget_us(job.request)) / 1000));
        if (job.request == Request::model) {
            // Before perform, rejection is local and has no remote effect.
            model_failure(job.output, AYNI_NOT_READY, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
        }
        if (self.current(job)) {
            if (esp_timer_get_time() < job.deadline) {
                self.perform(job);
            } else {
                std::snprintf(job.error.data(), job.error.size(), "请求排队超时，未发出");
            }
        }
        job.ready_us = uint64_t(esp_timer_get_time());
        if (job.request == Request::model) {
            deliver_model_completion(*self.transport_,
                                     {job.output,
                                      job.outline,
                                      {job.generation, job.http_complete_us, job.ready_us},
                                      job.deadline,
                                      job.ok});
        }
        // Request metadata only; never print transcripts, generated audio or keys.
        self.transport_->diagnostics().record(
            "wifi api request=%u ok=%u current=%u elapsed_ms=%lld error=%s\n",
            unsigned(job.request), unsigned(job.ok), unsigned(self.current(job)),
            static_cast<long long>(
                (esp_timer_get_time() - job.deadline + request_budget_us(job.request)) / 1000),
            job.error.data());
        std::fill(self.request_body_.begin(), self.request_body_.end(), 0);
        erase_free(job.audio, job.audio_bytes);
        job.audio = nullptr;
        job.audio_bytes = 0;
        // perform has returned: its client, parser and scratch borrows are gone. Other
        // owner/worker activity may overlap, so this is a heap snapshot, not leak attribution.
        self.transport_->diagnostics().record(
            "wifi api memory request=%u id=%llu internal_before=%zu internal_after=%zu "
            "internal_largest=%zu psram_free=%zu\n",
            unsigned(job.request), static_cast<unsigned long long>(job.input.id), internal_before,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        // Every queued slot reserves one result slot, so this cannot block on UI work.
        xQueueSend(self.done_, &index, portMAX_DELAY);
        self.outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }
}
bool Api::http(Job &job, const char *url, const char *content_type, const void *prefix,
               size_t prefix_bytes, const void *body, size_t body_bytes, const void *suffix,
               size_t suffix_bytes, size_t response_limit, CaptureReader *capture,
               SpeechStream *stream, ayni::providers::model_protocol::Stream *model_stream) {
    if (!request_clock_ready(url, std::time(nullptr))) {
        if (job.request == Request::model) {
            model_failure(job.output, AYNI_NOT_READY, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
        }
        std::snprintf(job.error.data(), job.error.size(), "网络尚未校时；请求未发出");
        return false;
    }
    const auto deadline = job.deadline;
    const ResponseLimit wire_limit{model_stream ? std::nullopt
                                                : std::optional<size_t>(response_limit)};
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
        if (job.request == Request::model) {
            model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE,
                          AYNI_CALL_EFFECT_NONE);
        }
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
    int last_read = 0;
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
    ok = ok && alive() && length >= 0 && status == 200 && wire_limit.accepts(uint64_t(length));
    if (!ok && job.request == Request::model && status == 200 && length >= 0 &&
        !wire_limit.accepts(uint64_t(length))) {
        model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
        std::snprintf(job.error.data(), job.error.size(), "模型响应超出缓冲上限；未重试");
    }
    if (!ok && status >= 100 && status <= 599 && status != 200) {
        if (job.request == Request::model) {
            const auto classified = status == 400                    ? AYNI_INVALID_ARGUMENT
                                    : status == 401 || status == 403 ? AYNI_DENIED
                                    : status == 404                  ? AYNI_NOT_FOUND
                                    : status == 408                  ? AYNI_EXPIRED
                                    : status == 429                  ? AYNI_BUSY
                                    : status >= 500                  ? AYNI_NOT_READY
                                                                     : AYNI_UNSUPPORTED;
            model_failure(job.output, classified);
        }
        std::snprintf(job.error.data(), job.error.size(), "API HTTP %d；不会自动重试", status);
    }
    // Only buffered JSON uses this allocation. Speech copies into the bounded ring;
    // model SSE discards consumed events and applies the shared parser's field bounds.
    const size_t capacity =
        length > 0 && length <= int64_t(response_limit) ? size_t(length) : response_limit;
    if (ok && !stream && !model_stream) {
        stage = "响应体";
        job.response = static_cast<uint8_t *>(
            heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!job.response) {
            if (job.request == Request::model) {
                model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            }
            std::snprintf(job.error.data(), job.error.size(), "API 响应内存不足");
            ok = false;
        }
    }
    if ((stream || model_stream) && ok) {
        stage = model_stream ? "模型流" : "语音流";
        bool first_body = true;
        std::array<uint8_t, network_chunk> chunk{};
        while (ok && alive()) {
            const int got =
                esp_http_client_read(client, reinterpret_cast<char *>(chunk.data()), chunk.size());
            last_read = got;
            if (got == -ESP_ERR_HTTP_EAGAIN) {
                continue;
            }
            if (got <= 0) {
                if (got < 0) {
                    transport_code = got;
                }
                ok = got == 0 && esp_http_client_is_complete_data_received(client);
                break;
            }
            if (first_body) {
                first_body = false;
                transport_->diagnostics().record(
                    "wifi api phase=first_body request=%u id=%llu elapsed_ms=%lld\n",
                    unsigned(job.request), static_cast<unsigned long long>(job.input.id),
                    static_cast<long long>(
                        (esp_timer_get_time() - job.deadline + request_budget_us(job.request)) /
                        1000));
            }
            ok = wire_limit.received(job.response_bytes, size_t(got));
            if (ok) {
                ok = stream ? stream->feed(chunk.data(), size_t(got))
                            : model_stream->feed({reinterpret_cast<const char *>(chunk.data()),
                                                  size_t(got)}) == AYNI_OK;
            }
            if (!ok && model_stream) {
                const auto parsed = model_stream->status();
                const auto cause = parsed == AYNI_OK ? AYNI_CAPACITY_EXCEEDED : parsed;
                model_failure(job.output, cause, AYNI_REASON_MODEL_OUTPUT);
                std::snprintf(job.error.data(), job.error.size(),
                              parsed == AYNI_OK ? "模型流字节计数溢出；未重试"
                              : cause == AYNI_CAPACITY_EXCEEDED ? "模型流保留数据超限；未重试"
                                                                : "模型流协议无效；未重试");
            }
        }
        std::fill(chunk.begin(), chunk.end(), 0);
        ok = ok && (!stream || stream->complete());
    }
    while (!stream && !model_stream && ok && alive()) {
        // fetch_headers may already have parsed and cached the whole body.
        // Parser completion is not proof that the application drained that cache.
        if (length > 0 && job.response_bytes == size_t(length)) {
            break;
        }
        if (job.response_bytes == capacity) {
            char excess = 0;
            const int count = esp_http_client_read(client, &excess, 1);
            last_read = count;
            if (count == -ESP_ERR_HTTP_EAGAIN) {
                continue;
            }
            ok = count == 0 && esp_http_client_is_complete_data_received(client);
            if (count < 0) {
                transport_code = count;
            }
            if (count > 0 && job.request == Request::model) {
                model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            }
            break;
        }
        const int got = esp_http_client_read(
            client, reinterpret_cast<char *>(job.response + job.response_bytes),
            std::min(network_chunk, capacity - job.response_bytes));
        last_read = got;
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
        int tls_code = 0, tls_flags = 0;
        const auto tls_status =
            esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        transport_->diagnostics().record(
            "wifi http stage=%s status=%d length=%lld code=%d errno=%d upload=%zu/%zu "
            "response=%zu parser_status=%ld read=%d complete=%u id=%llu\n",
            stage, status, static_cast<long long>(length), transport_code, socket_error, uploaded,
            expected_upload, job.response_bytes,
            static_cast<long>(model_stream ? model_stream->status() : AYNI_OK), last_read,
            unsigned(esp_http_client_is_complete_data_received(client)),
            static_cast<unsigned long long>(job.input.id));
        transport_->diagnostics().record(
            "wifi http_tls id=%llu status=%ld code=%d flags=%d internal_free=%zu "
            "internal_min=%zu internal_largest=%zu\n",
            static_cast<unsigned long long>(job.input.id), long(tls_status), tls_code, tls_flags,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    if (job.request == Request::template_capture) {
        model(job, nullptr); // Explicit local diagnostic reply; no endpoint or HTTP request.
        return;
    }
    if (job.request == Request::capacity) {
        capacity(job);
        return;
    }
    std::array<char, 256> url{};
    const char *root =
        job.request == Request::model ? settings_.model_url.data() : settings_.speech_url.data();
    const char *path = job.request == Request::model
                           ? ayni::providers::model_protocol::path(settings_.model_api()).data()
                       : job.request == Request::speech    ? "/v1/audio/speech/stream"
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
    case Request::capacity:
    case Request::template_capture:
        break; // Handled before endpoint selection; this job never sends HTTP.
    }
}
ayni::providers::model_protocol::RequestOptions Api::model_options(bool tools_allowed) const {
    ayni::providers::model_protocol::RequestOptions options;
    options.model = settings_.model.data();
    options.api = settings_.model_api();
    options.stream = settings_.model_streaming();
    options.chat_token_limit = settings_.model_token_limit();
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    options.continuation_scope = normalized_api_root(settings_.model_url.data());
    options.max_output_tokens = harness::max_output_tokens;
    options.result_format = tools_allowed ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT;
    if (std::strstr(settings_.model_url.data(), "https://api.deepseek.com") ==
        settings_.model_url.data()) {
        options.extra_parameters = R"({"thinking":{"type":"disabled"}})";
    }
    return options;
}
void Api::model(Job &job, const char *url) {
    // Before HTTP there is no remote effect. Each refusal keeps its concrete local cause.
    model_failure(job.output, AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
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
        model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
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
        model_failure(job.output,
                      status == AYNI_NOT_READY ? AYNI_EXPIRED
                      : status == AYNI_OK      ? AYNI_INVALID_ARGUMENT
                                               : status,
                      AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
        std::snprintf(job.error.data(), job.error.size(), "模型正文不可读、超时或已取消；未重试");
        return;
    }
    const ayni_string_v1 context{reinterpret_cast<const char *>(framed.data), reference.size};
    trace_tool_status(transport_->diagnostics(), context);
    if (tools_allowed && !context_probe_.ready()) {
        transport_->diagnostics().record("wifi context template_ready=%u\n",
                                         unsigned(context_probe_.capture(context)));
    }
    auto options = model_options(tools_allowed);
    const ayni::providers::model_protocol::Protocol codec;
    ModelBuffer json;
    auto out = ayni::json::Writer::growing(&json, [](void *opaque, size_t minimum) {
        auto &buffer = *static_cast<ModelBuffer *>(opaque);
        return buffer.grow(minimum, harness::model_body_bytes)
                   ? ayni::json::Writer::Buffer{reinterpret_cast<char *>(buffer.data), buffer.size}
                   : ayni::json::Writer::Buffer{};
    });
    size_t size = 0;
    const auto encoded = codec.write_request(out, options, context);
    if (encoded != AYNI_OK || !out.finish(size)) {
        model_failure(job.output, encoded != AYNI_OK ? encoded : AYNI_CAPACITY_EXCEEDED,
                      AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
        std::snprintf(job.error.data(), job.error.size(), "模型上下文编码失败或超限；未请求模型");
        return;
    }
    transport_->diagnostics().record(
        "wifi context framed_bytes=%lu wire_bytes=%zu wire_est_tokens=%llu "
        "internal_free=%zu internal_largest=%zu psram_free=%zu psram_largest=%zu\n",
        static_cast<unsigned long>(reference.size), size,
        static_cast<unsigned long long>(ayni::model::estimate_tokens(out.characters())),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    framed.reset(); // Only the projected JSON remains during the HTTP request.
    if (job.request == Request::template_capture) {
        if (!context_probe_.ready()) {
            std::snprintf(job.error.data(), job.error.size(), "诊断工具模板不可用；未调用模型");
            return;
        }
        // A local test fixture completes only the isolated diagnostic Task, never a user turn.
        const auto response =
            options.api == ayni::providers::model_protocol::Api::responses
                ? R"({"status":"completed","output":[{"type":"message","id":"diagnostic","role":"assistant","status":"completed","content":[{"type":"output_text","text":"local diagnostic only"}]}]})"
                : R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"local diagnostic only"}}]})";
        job.http_complete_us = uint64_t(esp_timer_get_time());
        job.ok = decode_model(response, tools_allowed, job.output, options, &job.outline);
        transport_->diagnostics().record("wifi probe template_only=1 no_model_http=1 status=%ld\n",
                                         long(job.ok ? AYNI_OK : AYNI_INVALID_ARGUMENT));
        return;
    }
    struct Delivery {
        Api &self;
        Job &job;
        bool enabled;
    } delivery{*this, job, tools_allowed};
    const ayni::providers::model_protocol::Preview preview{
        &delivery, [](void *context, std::string_view delta) {
            auto &d = *static_cast<Delivery *>(context);
            if (!d.enabled || !d.self.current(d.job)) {
                return;
            }
            auto &snapshot = d.self.preview_snapshot_;
            size_t take = std::min<size_t>(delta.size(), snapshot.text.size() - snapshot.size);
            while (take && take < delta.size() &&
                   (static_cast<unsigned char>(delta[take]) & 0xc0U) == 0x80U) {
                --take;
            }
            if (!take) {
                return;
            }
            std::memcpy(snapshot.text.data() + snapshot.size, delta.data(), take);
            snapshot.size += static_cast<uint32_t>(take);
            xQueueOverwrite(d.self.previews_, &snapshot); // UI-only tentative text, no Core event.
        }};
    preview_snapshot_ = {};
    preview_snapshot_.id = job.input.id;
    preview_snapshot_.generation = job.generation;
    std::unique_ptr<ayni::providers::model_protocol::Stream> stream;
    // This bounds retained parser data, not lifetime network traffic. Do not increase
    // memory allocation in proportion to cumulative SSE metadata.
    constexpr size_t stream_buffer_limit = json_limit * 4;
    const size_t response_limit = options.stream ? stream_buffer_limit : json_limit;
    if (options.stream) {
        stream.reset(new (std::nothrow)
                         ayni::providers::model_protocol::Stream(options, response_limit, preview));
    }
    if (options.stream && (!stream || stream->status() != AYNI_OK)) {
        model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
        std::snprintf(job.error.data(), job.error.size(), "模型流内存不足；未请求");
        return;
    }
    job.output.kind = {};
    job.output.size = 0; // From here a transport interruption is UNKNOWN.
    if (http(job, url, "application/json", nullptr, 0, json.data, size, nullptr, 0, response_limit,
             nullptr, nullptr, stream.get())) {
        job.http_complete_us = uint64_t(esp_timer_get_time());
        if (stream) {
            transport_->diagnostics().record(
                "wifi model_stream call=%llu http_complete=1 at_us=%llu wire_bytes=%zu "
                "buffer_limit=%zu\n",
                static_cast<unsigned long long>(job.input.id),
                static_cast<unsigned long long>(job.http_complete_us), job.response_bytes,
                stream_buffer_limit);
            auto answer = std::unique_ptr<ayni::providers::model_protocol::Answer>(
                new (std::nothrow) ayni::providers::model_protocol::Answer{});
            if (answer) {
                stream->finish(*answer);
                job.ok = package_model(options, *answer, tools_allowed, job.output, &job.outline);
            } else {
                model_failure(job.output, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            }
        } else {
            job.ok =
                decode_model({reinterpret_cast<const char *>(job.response), job.response_bytes},
                             tools_allowed, job.output, options, &job.outline);
        }
        if (!job.ok) {
            std::snprintf(job.error.data(), job.error.size(), "模型未返回完整有效回答，未重试");
        }
    }
    if (!job.ok && stream && stream->status() != AYNI_OK && current(job) &&
        job.output.kind != Kind::model_error) {
        model_failure(job.output, stream->status(), AYNI_REASON_MODEL_OUTPUT);
    }
    std::fill(preview_snapshot_.text.begin(), preview_snapshot_.text.end(), 0);
}
void Api::capacity(Job &job) {
    auto &log = transport_->diagnostics();
    if (!context_probe_.ready()) {
        std::snprintf(job.error.data(), job.error.size(), "先完成一次普通对话以取得当前工具模板");
        log.record("wifi probe status=%ld stage=template\n", long(AYNI_NOT_READY));
        return;
    }
    auto options = model_options(true);
    log.record("wifi probe begin context_tokens=%llu reserved_output_tokens=%llu body_limit=%lu\n",
               static_cast<unsigned long long>(harness::context_window_tokens),
               static_cast<unsigned long long>(harness::max_output_tokens),
               static_cast<unsigned long>(harness::model_body_bytes));
    for (auto corpus :
         {ContextProbe::Corpus::latin, ContextProbe::Corpus::han, ContextProbe::Corpus::escaped}) {
        uint32_t lower = 0, upper = harness::model_body_bytes;
        ContextProbe::Sample last, first_failure;
        while (upper - lower > 1) {
            if (!current(job) || esp_timer_get_time() >= job.deadline) {
                std::snprintf(job.error.data(), job.error.size(), "容量测试取消或到期，结果不完整");
                log.record("wifi probe interrupted corpus=%s\n", ContextProbe::name(corpus));
                return;
            }
            const auto units = lower + (upper - lower) / 2;
            auto sample = context_probe_.trial<ModelBuffer>(
                options, corpus, units, [&](const ContextProbe::Sample &peak) {
                    log.record(
                        "wifi probe peak corpus=%s units=%lu scratch_bytes=%zu "
                        "internal_free=%zu psram_free=%zu psram_largest=%zu stack_free=%lu\n",
                        ContextProbe::name(corpus), static_cast<unsigned long>(units),
                        peak.scratch_bytes,
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
                });
            if (sample.status == AYNI_OK) {
                lower = units;
                last = sample;
            } else {
                upper = units;
                first_failure = sample;
            }
            vTaskDelay(1); // Idle/motor/UI tasks retain CPU between bounded synthetic trials.
        }
        log.record("wifi probe boundary corpus=%s units_ok=%lu input_tokens=%llu framed_bytes=%zu "
                   "wire_bytes=%zu first_refused_units=%lu stage=%s status=%ld\n",
                   ContextProbe::name(corpus), static_cast<unsigned long>(lower),
                   static_cast<unsigned long long>(last.estimated_tokens), last.framed_bytes,
                   last.wire_bytes, static_cast<unsigned long>(upper), first_failure.stage,
                   long(first_failure.status));
    }
    job.ok = true;
    log.record("wifi probe complete; transport_only=1 no_model_http=1 history_unchanged=1\n");
}
void Api::speech(Job &job, const char *url) {
    const std::string_view input(reinterpret_cast<const char *>(job.input.body.data()),
                                 job.input.size);
    ayni::json::Writer out(request_body_.data(), request_body_.size());
    out.raw("{\"model\":\"qwen3-tts\",\"input\":");
    out.string(input);
    out.raw(",\"response_format\":\"pcm\",\"sample_rate\":24000}");
    size_t size = 0;
    if (!out.finish(size)) {
        std::snprintf(job.error.data(), job.error.size(), "朗读请求超限");
        return;
    }
    ModelBuffer storage(sizeof(SpeechStream));
    if (!storage.data) {
        std::snprintf(job.error.data(), job.error.size(), "语音流解析内存不足");
        return;
    }
    struct Delivery {
        Transport &transport;
        Api &api;
        Job &job;
        uint64_t call;
        uint32_t offset = 0;
    } delivery{*transport_, *this, job, job.input.id};
    auto sink = [](void *context, bool begin, const uint8_t *pcm, size_t bytes) {
        auto &d = *static_cast<Delivery *>(context);
        if (begin) {
            uint8_t header[8];
            put32(header, SpeechStream::rate);
            put32(header + 4, SpeechStream::max_pcm_bytes);
            return d.transport.post(Kind::audio_begin, d.call, header, sizeof(header));
        }
        while (bytes) {
            const auto count = std::min(network_chunk, bytes);
            while (true) {
                if (!d.api.current(d.job) || esp_timer_get_time() >= d.job.deadline) {
                    return false;
                }
                const auto accepted = d.transport.append_audio(d.call, d.offset, pcm, count);
                if (accepted == AudioWrite::accepted) {
                    break;
                }
                if (accepted == AudioWrite::rejected) {
                    return false;
                }
                vTaskDelay(
                    pdMS_TO_TICKS(5)); // full accepted zero bytes; wait, never replay output.
            }
            d.offset += count;
            pcm += count;
            bytes -= count;
        }
        return true;
    };
    auto *stream = new (storage.data) SpeechStream(sink, &delivery);
    const auto started = uint64_t(esp_timer_get_time());
    job.ok = http(job, url, "application/json", nullptr, 0, request_body_.data(), size, nullptr, 0,
                  SpeechStream::max_pcm_bytes + wav_overhead_limit, nullptr, stream);
    if (job.ok) {
        uint8_t total[sizeof(uint32_t)];
        put32(total, stream->received());
        job.ok = transport_->post(Kind::audio_end, job.input.id, total, sizeof(total));
    }
    if (!job.ok && stream->error()) {
        std::snprintf(job.error.data(), job.error.size(),
                      stream->error() == 3 ? "语音达到300秒上限；部分播放停止，未重播"
                      : stream->error() == 2 ? "语音生成预算耗尽；部分播放停止，未重播"
                                             : "语音生成流异常；部分播放停止，未重播");
    }
    const auto finished = uint64_t(esp_timer_get_time());
    const auto generation_error = stream->error();
    const auto pcm_bytes = stream->received();
    stream->~SpeechStream();
    if (!job.ok) {
        transport_->post(Kind::audio_error, job.input.id, nullptr, 0);
        if (!*job.error.data()) {
            std::snprintf(job.error.data(), job.error.size(), "语音流中断；已播放部分不会重试");
        }
    }
    const auto characters = std::count_if(input.begin(), input.end(),
                                          [](uint8_t byte) { return (byte & 0xc0) != 0x80; });
    const auto n = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    transport_->diagnostics().record(
        "wifi tts_segment call=%llu api_begin_us=%llu api_end_us=%llu bytes=%zu "
        "characters=%zu pcm_bytes=%lu server_error=%lu complete=%u\n",
        n(job.input.id), n(started), n(finished), input.size(), size_t(characters),
        static_cast<unsigned long>(pcm_bytes), static_cast<unsigned long>(generation_error),
        unsigned(job.ok));
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
    put32(wav + 24, capture::sample_rate);
    put32(wav + 28, capture::sample_rate * 2);
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
