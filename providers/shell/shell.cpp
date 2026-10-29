#include "process.h"
#include <ayni/providers/shell.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

extern char **environ;

namespace ayni::providers::shell {
namespace {
constexpr uint32_t capacity_max = 4;
constexpr uint32_t command_bytes_max = 16384;
constexpr uint32_t output_bytes_max = 32768;
constexpr uint32_t timeout_ms_max = 300000;
constexpr uint32_t encoded_metadata_bytes = 256;
constexpr auto retry_delay = std::chrono::milliseconds(2);
ayni_string_v1 text(const char *string) {
    return {string, static_cast<uint32_t>(std::strlen(string))};
}
bool same(ayni_string_v1 left, ayni_string_v1 right) {
    return left.size == right.size && left.data != nullptr && right.data != nullptr &&
           std::memcmp(left.data, right.data, left.size) == 0;
}
bool same_attempt(ayni_attempt_ref_v1 left, ayni_attempt_ref_v1 right) {
    return left.boot_epoch == right.boot_epoch && left.request_id == right.request_id;
}
} // namespace

struct Provider::Impl {
    enum class Phase { empty, queued, running, posting };
    struct Slot {
        Phase phase = Phase::empty;
        std::atomic<bool> cancelled{false};
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        std::unique_ptr<char[]> script;
    };
    Config config;
    ProcessConfig process;
    std::array<Slot, capacity_max> slots{};
    ayni_call_capability_v1 capability{};
    ayni_call_resource_v1 resource{};
    ayni_string_v1 unrestricted = text("shell.unrestricted");
    ayni_authority_field_desc_v1 domain{};
    std::array<ayni_schema_node_v1, 3> input_schema{};
    std::array<ayni_schema_node_v1, 7> result_schema{};
    std::array<ayni_string_v1, 6> end_tokens{text("exited"),       text("cancelled"),
                                             text("timeout"),      text("output_limit"),
                                             text("start_failed"), text("io_failed")};
    ayni_string_v1 base64_token = text("base64");
    ayni_value_node_v1 default_destination_node{};
    ayni_value_v1 default_destination{};
    ayni_authority_projection_v1 projection{};
    ayni_business_contract_v1 business{};
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    uint32_t active = 0;
    uint32_t next = 0;
    bool stopping = false;
    ayni_status_t worker_status = AYNI_OK;

    void descriptors() {
        resource = {text("shell.slot"), 1, 0};
        auto &contract = capability.contract;
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = text("system.shell");
        contract.struct_size = sizeof(contract);
        contract.form = AYNI_CALL_ACTION;
        contract.category = AYNI_CALL_TOOL;
        // An unrestricted script may make network requests or incur charges;
        // local process creation is not evidence of a local-only effect.
        contract.effects = AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING;
        contract.idempotency = AYNI_CALL_NOT_IDEMPOTENT;
        contract.query = AYNI_CALL_QUERY_NONE;
        contract.cancel = AYNI_CALL_CANCEL_REQUEST;
        contract.timeout_ms = config.timeout_ms;
        contract.input_format = AYNI_CALL_INPUT_VALUE;
        contract.result_format = AYNI_CALL_RESULT_VALUE;
        contract.input_text_limit = config.command_max_bytes + encoded_metadata_bytes;
        contract.result_text_limit =
            encoded_metadata_bytes + ((config.output_max_bytes + 2U) / 3U) * 4U;
        contract.resource_count = 1;
        contract.resources = &resource;
        domain.struct_size = sizeof(domain);
        domain.field_id = 1;
        domain.usage = AYNI_AUTHORITY_EGRESS_DESTINATION;
        domain.domain.struct_size = sizeof(domain.domain);
        domain.domain.kind = AYNI_AUTHORITY_IDENTIFIER;
        domain.domain.values.tokens = {&unrestricted, 1, 0};
        schemas();
    }

    void schemas() {
        input_schema[0].kind = AYNI_VALUE_OBJECT;
        input_schema[0].subtree_end = static_cast<uint32_t>(input_schema.size());
        input_schema[1].kind = AYNI_VALUE_TEXT;
        input_schema[1].field_id = 1;
        input_schema[1].subtree_end = 2;
        input_schema[1].flags = AYNI_SCHEMA_REQUIRED;
        input_schema[1].name = text("command");
        input_schema[1].min_count = 1;
        input_schema[1].max_count = config.command_max_bytes;
        input_schema[2].kind = AYNI_VALUE_IDENTIFIER;
        input_schema[2].field_id = 2;
        input_schema[2].subtree_end = 3;
        input_schema[2].name = text("destination");
        input_schema[2].choice_count = 1;
        input_schema[2].choices = &unrestricted;
        default_destination_node.kind = AYNI_VALUE_IDENTIFIER;
        default_destination_node.subtree_end = 1;
        default_destination_node.value.text = unrestricted;
        default_destination = {&default_destination_node, 1};
        input_schema[2].default_value = &default_destination;
        result_schema[0].kind = AYNI_VALUE_OBJECT;
        result_schema[0].subtree_end = static_cast<uint32_t>(result_schema.size());
        const std::array<ayni_string_v1, 6> names{text("end"),    text("exit_code"),
                                                  text("signal"), text("output_encoding"),
                                                  text("stdout"), text("stderr")};
        for (uint32_t at = 1; at < result_schema.size(); ++at) {
            auto &node = result_schema[at];
            node.field_id = at;
            node.subtree_end = at + 1U;
            node.flags = AYNI_SCHEMA_REQUIRED;
            node.name = names[at - 1U];
            node.kind = AYNI_VALUE_TEXT;
            node.max_count = capability.contract.result_text_limit;
        }
        result_schema[1].kind = AYNI_VALUE_ENUM;
        result_schema[1].choice_count = static_cast<uint32_t>(end_tokens.size());
        result_schema[1].choices = end_tokens.data();
        result_schema[2].kind = AYNI_VALUE_I64;
        result_schema[2].flags |= AYNI_SCHEMA_NULLABLE;
        result_schema[2].max_i64 = 255;
        result_schema[3].kind = AYNI_VALUE_I64;
        result_schema[3].max_i64 = std::numeric_limits<int>::max();
        result_schema[4].kind = AYNI_VALUE_ENUM;
        result_schema[4].choice_count = 1;
        result_schema[4].choices = &base64_token;
        projection.field_id = domain.field_id;
        projection.path_count = 1;
        projection.path[0] = 2;
        business = {{input_schema.data(), static_cast<uint32_t>(input_schema.size())},
                    {result_schema.data(), static_cast<uint32_t>(result_schema.size())},
                    1,
                    0,
                    &projection};
        capability.business = &business;
    }

    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (request == nullptr || completion == nullptr ||
            request->struct_size != sizeof(*request) || request->attempt.boot_epoch == 0U ||
            request->attempt.request_id == 0U ||
            request->capability_version != self.capability.version ||
            !same(request->capability_id, self.capability.capability_id) ||
            !same(request->endpoint_id, text("native.shell")) || request->value.node_count != 3U ||
            request->value.nodes == nullptr) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        const auto &command = request->value.nodes[1];
        const auto &destination = request->value.nodes[2];
        if (request->value.nodes[0].kind != AYNI_VALUE_OBJECT || command.field_id != 1U ||
            command.kind != AYNI_VALUE_TEXT || command.value.text.size == 0U ||
            command.value.text.size > self.config.command_max_bytes ||
            command.value.text.data == nullptr ||
            std::memchr(command.value.text.data, 0, command.value.text.size) != nullptr ||
            ayni_validate_utf8(command.value.text) != AYNI_OK || destination.field_id != 2U ||
            destination.kind != AYNI_VALUE_IDENTIFIER ||
            !same(destination.value.text, self.unrestricted)) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.stopping || self.worker_status != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t at = 0; at < self.config.request_capacity; ++at) {
            auto &slot = self.slots[at];
            if (slot.phase != Phase::empty) {
                continue;
            }
            std::memcpy(slot.script.get(), command.value.text.data, command.value.text.size);
            slot.script[command.value.text.size] = 0;
            slot.cancelled.store(false);
            slot.attempt = request->attempt;
            slot.completion = completion;
            slot.phase = Phase::queued;
            ++self.active;
            self.changed.notify_one();
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }

    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        for (uint32_t at = 0; at < self.config.request_capacity; ++at) {
            auto &slot = self.slots[at];
            if (slot.phase != Phase::empty && same_attempt(slot.attempt, attempt)) {
                slot.cancelled.store(true);
                self.changed.notify_one();
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }

    uint32_t select() {
        for (uint32_t offset = 0; offset < config.request_capacity; ++offset) {
            const uint32_t at = (next + offset) % config.request_capacity;
            if (slots[at].phase == Phase::queued) {
                slots[at].phase = Phase::running;
                next = (at + 1U) % config.request_capacity;
                return at;
            }
        }
        return capacity_max;
    }

    void complete(uint32_t index, const ProcessResult &result, const std::string &output,
                  const std::string &error) {
        auto &slot = slots[index];
        ayni_provider_result_v1 receipt{};
        receipt.struct_size = sizeof(receipt);
        receipt.attempt = slot.attempt;
        receipt.effect = result.started ? AYNI_CALL_EFFECT_UNKNOWN : AYNI_CALL_EFFECT_NONE;
        std::array<ayni_value_node_v1, 7> nodes{};
        nodes[0].kind = AYNI_VALUE_OBJECT;
        nodes[0].subtree_end = static_cast<uint32_t>(nodes.size());
        for (uint32_t at = 1; at < nodes.size(); ++at) {
            nodes[at].field_id = at;
            nodes[at].subtree_end = at + 1U;
            nodes[at].kind = result_schema[at].kind;
        }
        nodes[1].value.text = end_tokens[static_cast<size_t>(result.end)];
        if (result.end == ProcessResult::End::exited && result.signal == 0) {
            nodes[2].value.integer = result.exit_code;
        } else {
            nodes[2].kind = AYNI_VALUE_NULL;
        }
        nodes[3].value.integer = result.signal;
        nodes[4].value.text = base64_token;
        nodes[5].value.text = {output.data(), static_cast<uint32_t>(output.size())};
        nodes[6].value.text = {error.data(), static_cast<uint32_t>(error.size())};
        receipt.value = {nodes.data(), static_cast<uint32_t>(nodes.size())};
        switch (result.end) {
        case ProcessResult::End::exited:
            // COMPLETED means the shell exited and its exit/signal facts are
            // available, not that an arbitrary script achieved a business goal.
            receipt.outcome = AYNI_CALL_COMPLETED;
            receipt.status = AYNI_OK;
            receipt.effect = AYNI_CALL_EFFECT_COMPLETE;
            break;
        case ProcessResult::End::cancelled:
            receipt.outcome = AYNI_CALL_CANCELLED;
            receipt.status = AYNI_OK;
            break;
        case ProcessResult::End::timeout:
            receipt.outcome = AYNI_CALL_FAILED;
            receipt.status = AYNI_EXPIRED;
            break;
        case ProcessResult::End::output_limit:
            receipt.outcome = AYNI_CALL_FAILED;
            receipt.status = AYNI_CAPACITY_EXCEEDED;
            break;
        case ProcessResult::End::start_failed:
            receipt.outcome = AYNI_CALL_FAILED;
            receipt.status = AYNI_NOT_READY;
            break;
        case ProcessResult::End::io_failed:
            receipt.outcome = AYNI_CALL_ABANDONED;
            receipt.status = AYNI_RESULT_UNKNOWN;
            receipt.effect = AYNI_CALL_EFFECT_UNKNOWN;
            receipt.value = {};
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            slot.phase = Phase::posting;
        }
        for (;;) {
            const auto posted = ayni_provider_post(slot.completion, &receipt);
            std::unique_lock<std::mutex> lock(mutex);
            if (posted == AYNI_OK) {
                std::fill_n(slot.script.get(), config.command_max_bytes + 1U, '\0');
                slot.completion = nullptr;
                slot.phase = Phase::empty;
                --active; // Only after the last post returned; quiesce may now join safely.
                changed.notify_all();
                return;
            }
            if (posted != AYNI_BUSY && worker_status == AYNI_OK) {
                worker_status = posted; // Preserve failure; do not fabricate borrow release.
            }
            changed.wait_for(lock, retry_delay);
        }
    }

    void run() {
        for (;;) {
            uint32_t index = capacity_max;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || active != 0U; });
                if (stopping && active == 0U) {
                    return;
                }
                index = select();
            }
            if (index == capacity_max) {
                continue;
            }
            ProcessResult result;
            std::string output, error;
            try {
                result = execute(process, slots[index].script.get(), slots[index].cancelled);
                output = encode_output(result.standard_output);
                error = encode_output(result.standard_error);
            } catch (const std::bad_alloc &) {
                // An allocation failure may occur after fork; execute must own
                // child cleanup even when a bounded output allocation fails.
                result.end = ProcessResult::End::io_failed;
                result.started = true;
                std::lock_guard<std::mutex> lock(mutex);
                worker_status = AYNI_CAPACITY_EXCEEDED;
            }
            complete(index, result, output, error);
        }
    }
};

Provider::Provider() = default;
Provider::~Provider() {
    if (close() == AYNI_BUSY) {
        static_cast<void>(impl_.release()); // Caller violated the required drain/lifetime contract.
    }
}

ayni_status_t Provider::configure(const Config &config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if (!reaping_supported()) {
        return AYNI_UNSUPPORTED;
    }
    if (config.request_capacity == 0U || config.request_capacity > capacity_max ||
        config.command_max_bytes == 0U || config.command_max_bytes > command_bytes_max ||
        config.output_max_bytes == 0U || config.output_max_bytes > output_bytes_max ||
        config.timeout_ms == 0U || config.timeout_ms > timeout_ms_max ||
        config.working_directory.empty() || config.working_directory[0] != '/' ||
        config.working_directory.find('\0') != std::string::npos) {
        return AYNI_INVALID_ARGUMENT;
    }
    try {
        std::error_code error;
        if (!std::filesystem::is_directory(config.working_directory, error)) {
            return error ? AYNI_NOT_READY : AYNI_INVALID_ARGUMENT;
        }
        auto candidate = std::make_unique<Impl>();
        candidate->config = config;
        candidate->process.directory = config.working_directory;
        candidate->process.timeout_ms = config.timeout_ms;
        candidate->process.output_bytes = config.output_max_bytes;
        for (auto entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            candidate->process.environment.emplace_back(*entry);
        }
        for (auto &entry : candidate->process.environment) {
            candidate->process.environment_view.push_back(entry.data());
        }
        candidate->process.environment_view.push_back(nullptr);
        for (uint32_t at = 0; at < config.request_capacity; ++at) {
            candidate->slots[at].script = std::make_unique<char[]>(config.command_max_bytes + 1U);
        }
        candidate->descriptors();
        candidate->worker = std::thread([self = candidate.get()] { self->run(); });
        impl_ = std::move(candidate);
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    } catch (const std::system_error &) {
        return AYNI_NOT_READY;
    }
    return AYNI_OK;
}

ayni_status_t Provider::describe(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = {sizeof(out),
           1,
           text("native.shell"),
           text("native.shell"),
           impl_->unrestricted,
           impl_.get(),
           1,
           0,
           &impl_->capability,
           Impl::submit,
           Impl::cancel,
           nullptr,
           nullptr};
    return AYNI_OK;
}

ayni_status_t Provider::describe_authority(ayni_capability_desc_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = {sizeof(out), 1, impl_->capability.capability_id, 1, 0, &impl_->domain};
    return AYNI_OK;
}

uint32_t Provider::pending() const {
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active;
}

ayni_status_t Provider::status() const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->worker_status;
}

ayni_status_t Provider::quiesce() {
    if (!impl_) {
        return AYNI_OK;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->active != 0U) {
            return AYNI_BUSY;
        }
        impl_->stopping = true;
        impl_->changed.notify_one();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    return AYNI_OK;
}

ayni_status_t Provider::close() {
    const auto result = quiesce();
    if (result == AYNI_OK) {
        impl_.reset();
    }
    return result;
}
} // namespace ayni::providers::shell
