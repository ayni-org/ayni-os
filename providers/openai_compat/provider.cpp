#include "protocol.h"
#include <atomic>
#include <ayni/providers/openai_compat.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace ayni::providers::openai_compat {
namespace {
constexpr uint32_t request_capacity_max = 8;
constexpr uint32_t response_bytes_max = 1048576;
constexpr uint32_t timeout_max_ms = 300000;
constexpr uint32_t output_tokens_max = 8192;
constexpr size_t key_bytes_max = 4096;
constexpr size_t url_bytes_max = 2048;
constexpr size_t model_bytes_max = 128;
constexpr uint32_t contract_version = 1;
constexpr int poll_ms = 20;
using Clock = std::chrono::steady_clock;
ayni_string_v1 text(const std::string &value) {
    return {value.data(), static_cast<uint32_t>(value.size())};
}
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size &&
           (a.size == 0 || (a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0));
}
bool same(ayni_attempt_ref_v1 a, ayni_attempt_ref_v1 b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}
bool printable(const std::string &value, size_t maximum) {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c <= ' ' || c > '~') {
            return false;
        }
    }
    return true;
}
bool identifier(const std::string &value) {
    if (!printable(value, AYNI_CATALOG_ID_MAX_BYTES)) {
        return false;
    }
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return true;
}
// Environment names are process configuration, not Core catalog identities.
bool environment_name(const std::string &value) {
    if (!printable(value, model_bytes_max)) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i != 0 && c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}
// URL parsing stays in the adapter. No credential-bearing URL, query, fragment,
// redirect or ambient proxy can silently change the declared egress destination.
bool valid_url(const std::string &value) {
    if (!printable(value, url_bytes_max)) {
        return false;
    }
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK) {
        return false;
    }
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT}) {
        char *part_value = nullptr;
        const auto found = curl_url_get(url.get(), part, &part_value, 0);
        curl_free(part_value);
        if (found == CURLUE_OK) {
            return false;
        }
    }
    char *scheme = nullptr;
    char *host = nullptr;
    const auto scheme_status = curl_url_get(url.get(), CURLUPART_SCHEME, &scheme, 0);
    const auto host_status = curl_url_get(url.get(), CURLUPART_HOST, &host, 0);
    const bool allowed =
        scheme_status == CURLUE_OK && host_status == CURLUE_OK &&
        (std::strcmp(scheme, "https") == 0 ||
         (std::strcmp(scheme, "http") == 0 &&
          (std::strcmp(host, "127.0.0.1") == 0 || std::strcmp(host, "[::1]") == 0)));
    curl_free(scheme);
    curl_free(host);
    return allowed;
}
void wipe(char *data, size_t size) {
    volatile char *bytes = data;
    while (size != 0) {
        *bytes++ = 0;
        --size;
    }
}
} // namespace

struct Provider::Impl {
    // Owner writes an empty slot then release-publishes queued. Worker alone
    // advances queued->running->ready->empty. Cancellation is a separate control
    // request; it never frees a slot or overwrites a known terminal answer.
    enum class Phase { empty, queued, running, ready };
    struct Slot {
        std::atomic<Phase> phase{Phase::empty};
        std::atomic<bool> cancel_requested{false};
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        Clock::time_point deadline;
        uint32_t tool_count = 0;
        std::array<Tool, AYNI_GOAL_TYPE_TOOL_COUNT_MAX> tools;
        std::unique_ptr<char[]> input;
        uint32_t input_size = 0;
        std::unique_ptr<char[]> request;
        size_t request_size = 0;
        std::unique_ptr<char[]> response;
        size_t response_size = 0;
        size_t response_limit = 0;
        bool response_overflow = false; // Worker-owned callback evidence.
        bool cancel_control = false;    // Ready answer must publish this before its terminal.
        CURL *easy = nullptr;
        Answer answer;
        ~Slot() {
            if (easy) {
                curl_easy_cleanup(easy);
            }
        }
    };
    explicit Impl(const Config &value) : config(value), protocol(value.response_max_bytes) {}
    ~Impl() {
        stop();
        if (multi) {
            curl_multi_cleanup(multi);
        }
        slots.reset();
        // libcurl owns header copies; clear them before release, including the key.
        for (auto *header = headers; header; header = header->next) {
            wipe(header->data, std::strlen(header->data));
        }
        curl_slist_free_all(headers);
        if (global_initialized) {
            curl_global_cleanup();
        }
    }
    Config config;
    Protocol protocol;
    std::unique_ptr<Slot[]> slots;
    std::string url;
    CURLM *multi = nullptr;
    curl_slist *headers = nullptr;
    bool global_initialized = false;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> active{0};
    std::atomic<ayni_status_t> failure{AYNI_OK};
    ayni_call_resource_v1 resource{}; // model.slot admission remains owned by Core.
    ayni_call_capability_v1 capability{};
    ayni_provider_v1 descriptor{};

    void record_failure(ayni_status_t status) {
        auto expected = AYNI_OK;
        failure.compare_exchange_strong(expected, status);
    }
    void stop() {
        stopping.store(true);
        if (worker.joinable()) {
            worker.join();
        }
    }
    bool add_header(const char *header) {
        auto *added = curl_slist_append(headers, header);
        if (!added) {
            return false;
        }
        headers = added;
        return true;
    }
    // Read the secret only here, reject header injection, and retain it solely in
    // libcurl's private header list. Never include it in diagnostics (C-PROV-004).
    ayni_status_t prepare_headers() {
        const char *key = std::getenv(config.key_environment.c_str());
        if (!key || !*key) {
            return AYNI_NOT_READY;
        }
        const size_t key_size = strnlen(key, key_bytes_max + 1);
        if (key_size > key_bytes_max) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < key_size; ++i) {
            if (static_cast<unsigned char>(key[i]) <= ' ' ||
                static_cast<unsigned char>(key[i]) > '~') {
                return AYNI_INVALID_ARGUMENT;
            }
        }
        constexpr char prefix[] = "Authorization: Bearer ";
        std::array<char, sizeof(prefix) + key_bytes_max> authorization{};
        std::memcpy(authorization.data(), prefix, sizeof(prefix) - 1);
        std::memcpy(authorization.data() + sizeof(prefix) - 1, key, key_size);
        const bool authorized = add_header(authorization.data());
        wipe(authorization.data(), authorization.size());
        if (!authorized || !add_header("Content-Type: application/json") ||
            !add_header("Expect:")) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        return AYNI_OK;
    }
    ayni_status_t initialize() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return AYNI_INTERNAL;
        }
        global_initialized = true;
        const auto *version = curl_version_info(CURLVERSION_NOW);
        if (!(version->features & CURL_VERSION_THREADSAFE) ||
            !(version->features & CURL_VERSION_ASYNCHDNS)) {
            return AYNI_UNSUPPORTED;
        }
        if (!valid_url(config.base_url)) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto header_status = prepare_headers();
        if (header_status != AYNI_OK) {
            return header_status;
        }
        url = config.base_url;
        while (url.back() == '/') {
            url.pop_back();
        }
        url += "/chat/completions";
        multi = curl_multi_init();
        if (!multi) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        slots = std::make_unique<Slot[]>(config.request_capacity);
        for (uint32_t i = 0; i < config.request_capacity; ++i) {
            auto &slot = slots[i];
            slot.input = std::make_unique<char[]>(config.text_max_bytes);
            slot.request = std::make_unique<char[]>(request_json_max);
            slot.response = std::make_unique<char[]>(config.response_max_bytes);
            slot.answer.body = std::make_unique<char[]>(config.text_max_bytes);
            slot.response_limit = config.response_max_bytes;
            slot.easy = curl_easy_init();
            if (!slot.easy) {
                return AYNI_CAPACITY_EXCEEDED;
            }
        }
        build_descriptor();
        worker = std::thread([this] { run(); });
        return AYNI_OK;
    }
    void build_descriptor() {
        static constexpr char chat_id[] = "openai.chat";
        static constexpr char text_id[] = "openai.text";
        const ayni_string_v1 capability_id = config.result_format == AYNI_CALL_RESULT_TEXT
                                                 ? ayni_string_v1{text_id, sizeof(text_id) - 1}
                                                 : ayni_string_v1{chat_id, sizeof(chat_id) - 1};
        static constexpr char resource_id[] = "model.slot";
        resource = {{resource_id, sizeof(resource_id) - 1}, 1, 0};
        capability = {sizeof(capability),
                      contract_version,
                      capability_id,
                      {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL,
                       AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING, AYNI_CALL_NOT_IDEMPOTENT,
                       AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, config.timeout_ms,
                       AYNI_CALL_INPUT_FIELDS_TEXT, config.result_format, config.text_max_bytes,
                       config.text_max_bytes, 1, &resource, nullptr}};
        descriptor = {sizeof(descriptor),
                      contract_version,
                      text(config.provider_id),
                      text(config.endpoint_id),
                      text(config.destination_id),
                      this,
                      1,
                      0,
                      &capability,
                      submit,
                      cancel,
                      nullptr,
                      nullptr};
    }
    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (!request || !completion || request->struct_size != sizeof(*request) ||
            request->capability_version != contract_version || !request->attempt.boot_epoch ||
            !request->attempt.request_id || !request->logical_call_id || !request->operation_id ||
            !same(request->capability_id, self.capability.capability_id) ||
            !same(request->endpoint_id, self.descriptor.endpoint_id) ||
            request->text.size > self.config.text_max_bytes ||
            (request->text.size && !request->text.data) ||
            request->tool_count > AYNI_GOAL_TYPE_TOOL_COUNT_MAX ||
            (request->tool_count && !request->tool) || request->execution_permit ||
            self.stopping.load() || self.failure.load() != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t i = 0; i < self.config.request_capacity; ++i) {
            auto &slot = self.slots[i];
            if (slot.phase.load(std::memory_order_acquire) != Phase::empty) {
                continue;
            }
            for (uint32_t j = 0; j < request->tool_count; ++j) {
                ayni_tool_v1 tool{};
                tool.struct_size = sizeof(tool);
                if (request->tool(request->tool_context, j, &tool) != AYNI_OK ||
                    !slot.tools[j].copy(tool)) {
                    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
                }
            }
            if (request->text.size) {
                std::memcpy(slot.input.get(), request->text.data, request->text.size);
            }
            slot.input_size = request->text.size;
            slot.tool_count = request->tool_count;
            slot.attempt = request->attempt;
            slot.completion = completion;
            slot.deadline = Clock::now() + std::chrono::milliseconds(self.config.timeout_ms);
            slot.cancel_requested.store(false);
            slot.cancel_control = false;
            self.active.fetch_add(1);
            slot.phase.store(Phase::queued, std::memory_order_release);
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        for (uint32_t i = 0; i < self.config.request_capacity; ++i) {
            auto &slot = self.slots[i];
            if (slot.phase.load(std::memory_order_acquire) != Phase::empty &&
                same(slot.attempt, attempt)) {
                slot.cancel_requested.store(true);
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static size_t receive(char *data, size_t width, size_t count, void *context) {
        auto &slot = *static_cast<Slot *>(context);
        if (width != 0 && count > (slot.response_limit - slot.response_size) / width) {
            slot.response_overflow = true;
            return 0;
        }
        const size_t size = width * count;
        std::memcpy(slot.response.get() + slot.response_size, data, size);
        slot.response_size += size;
        return size;
    }
    bool setup(Slot &slot, long remaining_ms) {
        curl_easy_reset(slot.easy);
        // No automatic retry/fallback. Fresh connections also avoid implicit reuse retries.
        return curl_easy_setopt(slot.easy, CURLOPT_URL, url.c_str()) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_POST, 1L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_POSTFIELDS, slot.request.get()) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_POSTFIELDSIZE_LARGE,
                                static_cast<curl_off_t>(slot.request_size)) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_WRITEFUNCTION, receive) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_WRITEDATA, &slot) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_PRIVATE, &slot) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_TIMEOUT_MS, remaining_ms) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_CONNECTTIMEOUT_MS, remaining_ms) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_PROXY, "") == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_NETRC, CURL_NETRC_IGNORED) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_PROTOCOLS_STR, "http,https") == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) ==
                   CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_FRESH_CONNECT, 1L) == CURLE_OK &&
               curl_easy_setopt(slot.easy, CURLOPT_FORBID_REUSE, 1L) == CURLE_OK;
    }
    void prepare(Slot &slot) {
        slot.answer.result = {};
        slot.answer.result.struct_size = sizeof(ayni_provider_result_v1);
        slot.answer.result.attempt = slot.attempt;
        slot.answer.used = 0;
        slot.answer.fail(AYNI_INTERNAL, AYNI_REASON_NONE);
        slot.response_size = 0;
        slot.response_overflow = false;
        slot.phase.store(Phase::ready);
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(slot.deadline - Clock::now())
                .count();
        if (remaining <= 0) {
            slot.answer.fail(AYNI_EXPIRED, AYNI_REASON_NONE);
            slot.answer.result.effect = AYNI_CALL_EFFECT_NONE;
            return;
        }
        const auto encoded =
            protocol.encode(config, {slot.input.get(), slot.input_size}, slot.tools.data(),
                            slot.tool_count, slot.request.get(), slot.request_size);
        if (encoded != AYNI_OK) {
            slot.answer.fail(encoded, AYNI_REASON_NONE);
            slot.answer.result.effect = AYNI_CALL_EFFECT_NONE;
            return;
        }
        if (!setup(slot, static_cast<long>(remaining)) ||
            curl_multi_add_handle(multi, slot.easy) != CURLM_OK) {
            slot.answer.result.effect = AYNI_CALL_EFFECT_NONE;
            record_failure(AYNI_INTERNAL);
            return;
        }
        slot.phase.store(Phase::running);
    }
    bool detach(Slot &slot) {
        if (curl_multi_remove_handle(multi, slot.easy) == CURLM_OK) {
            return true;
        }
        retire_transport();
        return false;
    }
    void abandon(Slot &slot) {
        slot.answer.result = {};
        slot.answer.result.struct_size = sizeof(ayni_provider_result_v1);
        slot.answer.result.attempt = slot.attempt;
        slot.answer.fail(AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
        slot.answer.result.outcome = AYNI_CALL_ABANDONED;
        slot.phase.store(Phase::ready);
    }
    void cancel_work(Slot &slot) {
        if (slot.phase.load() == Phase::running && !detach(slot)) {
            return;
        }
        abandon(slot);
        slot.cancel_control = true; // C-PROV-006: disconnect cannot establish remote nonexecution.
    }
    void completed(Slot &slot, CURLcode code) {
        if (!detach(slot)) {
            return;
        }
        slot.phase.store(Phase::ready);
        if (slot.response_overflow) {
            slot.answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        if (code != CURLE_OK) {
            abandon(slot);
            return;
        }
        long http = 0;
        if (curl_easy_getinfo(slot.easy, CURLINFO_RESPONSE_CODE, &http) != CURLE_OK) {
            abandon(slot);
            return;
        }
        constexpr long http_ok = 200, http_bad_request = 400, http_unauthorized = 401,
                       http_forbidden = 403, http_not_found = 404, http_timeout = 408,
                       http_rate_limit = 429, http_server_error = 500;
        if (http == http_ok) {
            protocol.decode(slot.response.get(), slot.response_size, slot.tools.data(),
                            slot.tool_count, config.text_max_bytes, slot.answer);
        } else {
            const auto status = http == http_bad_request ? AYNI_INVALID_ARGUMENT
                                : http == http_unauthorized || http == http_forbidden ? AYNI_DENIED
                                : http == http_not_found    ? AYNI_NOT_FOUND
                                : http == http_timeout      ? AYNI_EXPIRED
                                : http == http_rate_limit   ? AYNI_BUSY
                                : http >= http_server_error ? AYNI_NOT_READY
                                                            : AYNI_UNSUPPORTED;
            slot.answer.fail(status, AYNI_REASON_NONE);
        }
    }
    void publish(Slot &slot) {
        if (slot.cancel_control) {
            auto control = slot.answer.result;
            control.outcome = AYNI_CALL_CANCEL_UNKNOWN;
            const auto posted = ayni_provider_post(slot.completion, &control);
            if (posted != AYNI_OK) {
                if (posted != AYNI_BUSY) {
                    record_failure(posted);
                }
                return;
            }
            slot.cancel_control = false;
        }
        auto posted = ayni_provider_post(slot.completion, &slot.answer.result);
        if (posted == AYNI_CAPACITY_EXCEEDED || posted == AYNI_INVALID_ARGUMENT) {
            // The ABI owns list encoding/size. Do not duplicate its wire-size formula.
            slot.answer.fail(posted, AYNI_REASON_MODEL_OUTPUT);
            posted = ayni_provider_post(slot.completion, &slot.answer.result);
        }
        if (posted != AYNI_OK) {
            if (posted != AYNI_BUSY) {
                record_failure(posted);
            }
            return;
        }
        wipe(slot.input.get(), slot.input_size);
        wipe(slot.request.get(), slot.request_size);
        wipe(slot.response.get(), slot.response_size);
        wipe(slot.answer.body.get(), slot.answer.used);
        active.fetch_sub(1);
        slot.phase.store(Phase::empty, std::memory_order_release);
    }
    // A fatal multi error closes transport, but the worker remains alive: submit
    // may already be copying a view when failure becomes visible (ADR-0055).
    void retire_transport() {
        record_failure(AYNI_INTERNAL);
        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
        for (uint32_t i = 0; i < config.request_capacity; ++i) {
            if (slots[i].phase.load() == Phase::running) {
                abandon(slots[i]);
            }
        }
    }
    void drive_transport() {
        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            retire_transport();
            return;
        }
        int remaining = 0;
        while (auto *message = curl_multi_info_read(multi, &remaining)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            Slot *slot = nullptr;
            if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &slot) != CURLE_OK ||
                !slot) {
                retire_transport();
                return;
            }
            completed(*slot, message->data.result);
            if (!multi) {
                return;
            }
        }
        if (curl_multi_poll(multi, nullptr, 0, poll_ms, nullptr) != CURLM_OK) {
            retire_transport();
        }
    }
    void run() {
        while (!stopping.load()) {
            for (uint32_t i = 0; i < config.request_capacity; ++i) {
                auto &slot = slots[i];
                const auto phase = slot.phase.load(std::memory_order_acquire);
                if (phase == Phase::empty) {
                    continue;
                }
                if (phase != Phase::ready && slot.cancel_requested.load()) {
                    cancel_work(slot);
                } else if (phase == Phase::queued) {
                    if (multi) {
                        prepare(slot);
                    } else {
                        abandon(slot);
                    }
                }
                if (slot.phase.load() == Phase::ready) {
                    publish(slot);
                }
            }
            if (multi) {
                drive_transport();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            }
        }
        // Shutdown is owner-thread only, so no new submit can race this final pass.
        // Keep a failed publication borrowed: close/status must expose the failure.
        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
        for (uint32_t i = 0; slots && i < config.request_capacity; ++i) {
            auto &slot = slots[i];
            const auto phase = slot.phase.load();
            if (phase == Phase::empty) {
                continue;
            }
            if (phase != Phase::ready) {
                abandon(slot);
            }
            publish(slot);
        }
    }
};

Provider::Provider() = default;
Provider::~Provider() {
    if (impl_) {
        impl_->stop();
        if (impl_->active.load() != 0) {
            static_cast<void>(impl_.release());
        }
    }
}
ayni_status_t Provider::configure(const Config &config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if ((config.result_format != AYNI_CALL_RESULT_TEXT &&
         config.result_format != AYNI_CALL_RESULT_TOOL_CALLS) ||
        !config.request_capacity || config.request_capacity > request_capacity_max ||
        !config.text_max_bytes || config.text_max_bytes > AYNI_CALL_TEXT_BYTES_MAX ||
        !config.response_max_bytes || config.response_max_bytes > response_bytes_max ||
        !config.timeout_ms || config.timeout_ms > timeout_max_ms || !config.max_output_tokens ||
        config.max_output_tokens > output_tokens_max ||
        !printable(config.base_url, url_bytes_max) || !printable(config.model, model_bytes_max) ||
        !environment_name(config.key_environment) || !identifier(config.provider_id) ||
        !identifier(config.endpoint_id) || !identifier(config.destination_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    try {
        auto created = std::make_unique<Impl>(config);
        const auto status = created->initialize();
        if (status != AYNI_OK) {
            return status;
        }
        impl_ = std::move(created);
        return AYNI_OK;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    } catch (const std::system_error &) {
        return AYNI_INTERNAL;
    }
}
ayni_status_t Provider::describe(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = impl_->descriptor;
    return AYNI_OK;
}
ayni_status_t Provider::close() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    if (impl_->active.load() != 0) {
        return AYNI_BUSY;
    }
    impl_->stop();
    const auto status = impl_->failure.load();
    impl_.reset();
    return status;
}
ayni_status_t Provider::quiesce() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    if (impl_->active.load() != 0) {
        return AYNI_BUSY;
    }
    impl_->stop();
    return AYNI_OK;
}
uint32_t Provider::pending() const {
    return impl_ ? impl_->active.load() : 0;
}
ayni_status_t Provider::status() const {
    return impl_ ? impl_->failure.load() : AYNI_INVALID_STATE;
}
} // namespace ayni::providers::openai_compat
