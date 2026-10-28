#include "fixture_model.h"
#include "fixture_call_barrier.h"
namespace ayni::runtime::fixtures {
namespace {
constexpr uint32_t text_limit = 512;
}
ayni_status_t ModelProbe::configure(providers::mock::Variant variant, uint64_t delay_us,
                                    Clock clock, std::optional<uint32_t> timeout_ms) {
    clock_ = clock;
    if (timeout_ms && *timeout_ms == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    delay_us_ = delay_us;
    const auto status =
        provider_.configure({variant, providers::mock::Service::text, 1, text_limit, {}});
    if (status != AYNI_OK) {
        return status;
    }
    ayni_provider_v1 offer{};
    const auto described = provider_.describe(offer);
    if (described == AYNI_OK) {
        if (timeout_ms && *timeout_ms > offer.capabilities->contract.timeout_ms) {
            const auto closed = provider_.close();
            return closed == AYNI_OK ? AYNI_INVALID_ARGUMENT : closed;
        }
        original_ = offer;
        declaration_ = *offer.capabilities;
        if (timeout_ms) {
            declaration_.contract.timeout_ms = *timeout_ms;
        }
    }
    return described;
}
ayni_provider_v1 ModelProbe::describe() {
    auto offer = *original_;
    offer.ctx = this;
    offer.capabilities = &declaration_;
    offer.submit = submit;
    offer.cancel = cancel;
    return offer;
}
ayni_submit_result_t ModelProbe::submit(void *ctx, const ayni_provider_request_v1 *request,
                                        ayni_provider_completion_t *completion) {
    auto &self = *static_cast<ModelProbe *>(ctx);
    if (self.dispatch_) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto now = self.clock_.now_us(self.clock_.context);
    if (self.delay_us_ > UINT64_MAX - now) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto status = self.original_->submit(self.original_->ctx, request, completion);
    if (status != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        self.dispatch_ =
            Dispatch{request->logical_call_id, now + self.delay_us_, DispatchPhase::queued};
    }
    return status;
}
ayni_submit_result_t ModelProbe::cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<ModelProbe *>(ctx);
    if (self.cancellation_ == Cancellation::late_success) {
        return AYNI_SUBMIT_ACCEPTED;
    }
    const auto status = self.original_->cancel(self.original_->ctx, attempt);
    if (self.dispatch_ && status != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        self.dispatch_->due_us = self.clock_.now_us(self.clock_.context);
        self.dispatch_->phase = DispatchPhase::cancelled;
    }
    return status;
}
ayni_status_t ModelProbe::advance(ayni_kernel_t *kernel, uint64_t &barrier_checks) {
    if (!dispatch_) {
        return AYNI_OK;
    }
    if (clock_.now_us(clock_.context) < dispatch_->due_us) {
        return AYNI_OK;
    }
    if (dispatch_->phase == DispatchPhase::queued) {
        const auto status = check_barrier(kernel, dispatch_->call);
        if (status != AYNI_OK) {
            return status;
        }
        dispatch_->phase = DispatchPhase::checked;
        ++barrier_checks;
    }
    const auto status = provider_.advance();
    if (status == AYNI_BUSY) {
        return AYNI_OK;
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (!provider_.pending()) {
        dispatch_.reset();
    }
    return AYNI_OK;
}
ayni_status_t ModelProbe::set_cancellation(Cancellation mode) {
    if (dispatch_) {
        return AYNI_BUSY;
    }
    cancellation_ = mode;
    return AYNI_OK;
}
ayni_status_t ModelProbe::set_delay(uint64_t delay_us) {
    if (dispatch_) {
        return AYNI_BUSY;
    }
    delay_us_ = delay_us;
    return AYNI_OK;
}
ayni_status_t ModelProbe::close() {
    if (!original_) {
        return AYNI_OK;
    }
    const auto status = provider_.close();
    if (status == AYNI_OK) {
        original_.reset();
    }
    return status;
}
void ModelProbe::abandon_kernel() {
    provider_.abandon_kernel();
    dispatch_.reset();
}
} // namespace ayni::runtime::fixtures
