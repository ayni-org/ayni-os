// Owner-thread command identities and durable receipts over the public C API.
#ifndef AYNI_COMMANDS_HPP
#define AYNI_COMMANDS_HPP
#include <ayni/ayni.hpp>
#include <ayni/host_authority.h>
namespace ayni {
// One monotonic management identity stream; multi-command applications may share this
// mechanism while retaining their own bounded receipt window. Never a Kernel object ID.
class CommandSequence {
  public:
    CommandSequence() = default; // Unbound; zero is never an emitted identity.
    CommandSequence(const CommandSequence &) = delete;            // A stream has exactly one owner.
    CommandSequence &operator=(const CommandSequence &) = delete; // No copied sequence.
    // Rebinding the same boot preserves the ordinal; another boot needs a new stream owner.
    ayni_status_t configure(uint64_t boot) {
        if (!boot || boot >= UINT64_MAX / command_range) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (boot_ && boot_ != boot) {
            return AYNI_INVALID_STATE;
        }
        boot_ = boot;
        return AYNI_OK;
    }
    // Zero means unbound/exhausted. Refused submissions still consume their unique ordinal.
    uint64_t next() {
        return !boot_ || sequence_ == command_range - 1 ? 0 : boot_ * command_range + ++sequence_;
    }

  private:
    static constexpr uint32_t sequence_bits = 32;
    static constexpr uint64_t command_range = UINT64_C(1) << sequence_bits;
    uint64_t boot_ = 0;     // Durable boot epoch; zero before configure.
    uint64_t sequence_ = 0; // Boot-local ordinal; never reset when the same boot is configured.
};
// One client per principal command stream. The composition binds the current boot/session
// after opening its authenticated session; all other producers must use disjoint identities.
// No I/O, retries of accepted commands, allocation, global registry or Kernel ownership.
class CommandClient {
  public:
    CommandClient() = default; // Unbound; no command identities have been consumed.
    CommandClient(const CommandClient &) = delete;            // Identities have one owner.
    CommandClient &operator=(const CommandClient &) = delete; // Never copy a command stream.
    // Binds once; borrowed Kernel/session remain valid through the last poll/submit.
    ayni_status_t configure(ayni_kernel_t *kernel, uint64_t principal, uint64_t session,
                            uint64_t boot) {
        if (kernel_) {
            return AYNI_INVALID_STATE;
        }
        if (!kernel || !principal || !session) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto status = identities_.configure(boot);
        if (status != AYNI_OK) {
            return status;
        }
        kernel_ = kernel;
        principal_ = principal;
        session_ = session;
        return AYNI_OK;
    }
    // Observes the accepted identity; only a durable receipt reopens admission.
    ayni_status_t poll() {
        if (!pending_) {
            return AYNI_OK;
        }
        auto receipt = initialized<ayni_intent_receipt_v1>();
        const auto status = ayni_get_intent_receipt(kernel_, principal_, pending_, &receipt);
        if (status == AYNI_OK && receipt.phase == AYNI_INTENT_DURABLE) {
            pending_ = 0;
        }
        return status;
    }
    // Synchronously copies the intent body via Core. A caller retries only BUSY/refusal;
    // accepted commands are observed through poll, never resubmitted with a new identity.
    Submission submit(ayni_intent_kind_t kind, const void *body, uint32_t revision = 0) {
        Submission out;
        out.status = poll();
        if (out.status != AYNI_OK) {
            return out;
        }
        if (pending_) {
            out.status = AYNI_BUSY;
            return out;
        }
        const auto command_id = identities_.next();
        if (!kernel_ || !command_id) {
            out.status = AYNI_NOT_READY;
            return out;
        }
        const ayni_intent_v1 intent{sizeof(intent), kind, command_id, session_, revision, 0, body};
        out.status = ayni_submit_intent(kernel_, &intent, &out.result, &out.error);
        if (out.status == AYNI_OK && out.result.phase != AYNI_INTENT_DURABLE) {
            pending_ = intent.command_id;
        }
        return out;
    }
    // Zero means no accepted command is waiting for durability; never a Core lifecycle flag.
    uint64_t pending() const { return pending_; }

  private:
    CommandSequence identities_;           // Owns this principal's identity interval.
    ayni_kernel_t *kernel_ = nullptr;      // Borrowed, null before configure; never destroyed here.
    uint64_t principal_ = 0, session_ = 0; // Immutable configured identities; zero unbound.
    uint64_t pending_ =
        0; // Exact accepted command awaiting durability; zero when admission is open.
};
} // namespace ayni
#endif
