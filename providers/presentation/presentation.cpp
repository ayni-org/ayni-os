#include <array>
#include <ayni/providers/presentation.h>
#include <ayni/values.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
namespace ayni::providers::presentation {
namespace {
constexpr uint32_t notification_timeout_ms =
    172800000; // Finite 48 h covers one <24 h quiet interval.
constexpr auto receipt_retry_delay = std::chrono::milliseconds(2);
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0;
}
} // namespace
uint32_t call_input_bytes() {
    return text_value_bytes(text_bytes_max);
}
struct Provider::Impl {
    enum class Phase { empty, queued, checking, writing, cancelled, terminal };
    struct Slot {
        Phase phase = Phase::empty;
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        uint32_t size = 0;
        std::array<char, text_bytes_max> body{};
        ayni_provider_result_v1 receipt{};
    };
    Output output;
    Availability availability;
    std::array<Slot, request_capacity> slots{};
    std::array<ayni_call_resource_v1, 2> resources{};
    ayni_call_capability_v1 capability{};
    ayni_schema_node_v1 input_schema{}, result_schema{};
    ayni_business_contract_v1 business{};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool available = false, stopping = false;
    ayni_status_t worker_status = AYNI_OK;
    uint32_t active = 0;

    void descriptor() {
        resources = {{{text("notification.slot"), 1, 0}, {text("notification.window"), 1, 0}}};
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = text("notify.present");
        capability.contract = {sizeof(ayni_call_contract_v1),
                               AYNI_CALL_ACTION,
                               AYNI_CALL_TOOL,
                               AYNI_CALL_EFFECT_NOTIFICATION,
                               AYNI_CALL_NOT_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_REQUEST,
                               0,
                               notification_timeout_ms,
                               AYNI_CALL_INPUT_VALUE,
                               AYNI_CALL_RESULT_TEXT,
                               call_input_bytes(),
                               call_result_bytes,
                               static_cast<uint32_t>(resources.size()),
                               resources.data(),
                               nullptr,
                               AYNI_CALL_DURABILITY_DURABLE,
                               0};
        input_schema.kind = result_schema.kind = AYNI_VALUE_TEXT;
        input_schema.subtree_end = result_schema.subtree_end = 1;
        input_schema.max_count = text_bytes_max;
        business = {{&input_schema, 1}, {&result_schema, 1}, 0, 0, nullptr};
        capability.business = &business;
    }
    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *r,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (!r || !completion || r->struct_size != sizeof(*r) || !r->attempt.boot_epoch ||
            !r->attempt.request_id || !same(r->capability_id, self.capability.capability_id) ||
            r->capability_version != 1 || !same(r->endpoint_id, text("channel.output"))) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        ayni::ValueReader input;
        ayni_string_v1 body{};
        if (input.read(r->value) != AYNI_OK || !ayni::ValueReader::text(input.root(), body) ||
            body.size > text_bytes_max) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.stopping || self.worker_status != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (auto &slot : self.slots) {
            if (slot.phase != Phase::empty) {
                continue;
            }
            slot.phase = Phase::queued;
            slot.attempt = r->attempt;
            slot.completion = completion;
            slot.size = body.size;
            if (slot.size) {
                std::memcpy(slot.body.data(), body.data, slot.size);
            }
            ++self.active;
            self.changed.notify_one();
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        for (auto &slot : self.slots) {
            if (slot.phase == Phase::empty || slot.attempt.boot_epoch != attempt.boot_epoch ||
                slot.attempt.request_id != attempt.request_id) {
                continue;
            }
            if (slot.phase == Phase::queued || slot.phase == Phase::checking) {
                slot.phase = Phase::cancelled;
            }
            // Writing already began: cancellation cannot retract bytes. The original
            // terminal receipt remains owed, with its actual effect (ADR-0066).
            self.changed.notify_one();
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    void finish_write(Slot &slot, bool cancelled) {
        // No lock while a channel writes or a terminal post calls Host wake.
        auto status = AYNI_OK;
        try {
            if (!cancelled) {
                status = output.write(output.context, {slot.body.data(), slot.size});
            }
        } catch (...) {
            status = AYNI_RESULT_UNKNOWN;
        } // Preserve the completion even for a broken callback.
        auto &r = slot.receipt;
        r = {};
        r.struct_size = sizeof(r);
        r.attempt = slot.attempt;
        r.outcome = cancelled                    ? AYNI_CALL_CANCELLED
                    : status == AYNI_OK          ? AYNI_CALL_COMPLETED
                    : status == AYNI_UNSUPPORTED ? AYNI_CALL_FAILED
                                                 : AYNI_CALL_ABANDONED;
        r.status = status == AYNI_OK            ? AYNI_OK
                   : status == AYNI_UNSUPPORTED ? AYNI_UNSUPPORTED
                                                : AYNI_RESULT_UNKNOWN;
        r.effect = cancelled || status == AYNI_UNSUPPORTED ? AYNI_CALL_EFFECT_NONE
                   : status == AYNI_OK                     ? AYNI_CALL_EFFECT_COMPLETE
                                                           : AYNI_CALL_EFFECT_UNKNOWN;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            Slot *selected = nullptr;
            for (auto &slot : slots) {
                if (slot.phase == Phase::terminal || slot.phase == Phase::cancelled ||
                    (available && slot.phase == Phase::queued)) {
                    selected = &slot;
                    break;
                }
            }
            if (!selected) {
                if (stopping) {
                    return;
                }
                changed.wait(lock);
                continue;
            }
            auto &slot = *selected;
            if (slot.phase != Phase::terminal) {
                bool cancelled = slot.phase == Phase::cancelled;
                slot.phase = cancelled ? Phase::cancelled : Phase::checking;
                lock.unlock();
                bool permitted = true;
                auto gate_status = AYNI_OK;
                if (!cancelled && availability.read) {
                    gate_status = availability.read(availability.context, &permitted);
                }
                lock.lock();
                cancelled = cancelled || slot.phase == Phase::cancelled;
                if (!cancelled && (gate_status != AYNI_OK || !permitted)) {
                    // Still no channel bytes: keep the original operation queued. A
                    // cancel accepted during the check wins before any future write.
                    slot.phase = Phase::queued;
                    if (gate_status != AYNI_OK) {
                        worker_status = gate_status;
                        return;
                    }
                    changed.wait_for(lock, receipt_retry_delay);
                    continue;
                }
                slot.phase = Phase::writing; // Gate linearization: in-flight output may finish.
                lock.unlock();
                finish_write(slot, cancelled);
                lock.lock();
                slot.phase = Phase::terminal;
            }
            auto *completion = slot.completion;
            lock.unlock();
            const auto status = ayni_provider_post(completion, &slot.receipt);
            lock.lock();
            if (status == AYNI_OK) {
                slot.phase = Phase::empty;
                slot.completion = nullptr;
                --active;
            } else if (status != AYNI_BUSY) {
                worker_status = status;
                return; // Retain the borrow and diagnostic; owner must not fabricate drain.
            } else {
                // Retry this receipt only. Never call the channel twice for the operation.
                changed.wait_for(lock, receipt_retry_delay);
            }
        }
    }
};
Provider::Provider() = default;
Provider::~Provider() {
    if (impl_ && quiesce() != AYNI_OK) {
        static_cast<void>(impl_.release());
    }
}
ayni_status_t Provider::configure(Output output, Availability availability) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if (!output.write) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::unique_ptr<Impl> candidate(new (std::nothrow) Impl);
    if (!candidate) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    candidate->output = output;
    candidate->availability = availability;
    candidate->descriptor();
    try {
        candidate->worker = std::thread([self = candidate.get()] { self->run(); });
    } catch (const std::system_error &) {
        return AYNI_CAPACITY_EXCEEDED;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    impl_ = std::move(candidate);
    return AYNI_OK;
}
ayni_provider_v1 Provider::describe() {
    if (!impl_) {
        return {};
    }
    return {sizeof(ayni_provider_v1),
            1,
            text("channel.presentation"),
            text("channel.output"),
            {},
            impl_.get(),
            1,
            0,
            &impl_->capability,
            Impl::submit,
            Impl::cancel,
            nullptr,
            nullptr};
}
ayni_status_t Provider::advance() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->changed.notify_one();
    return impl_->worker_status;
}
void Provider::set_available(bool available) {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->available = available;
    impl_->changed.notify_one();
}
uint32_t Provider::pending() const {
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active;
}
ayni_status_t Provider::quiesce() {
    if (!impl_) {
        return AYNI_OK;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->active) {
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
} // namespace ayni::providers::presentation
