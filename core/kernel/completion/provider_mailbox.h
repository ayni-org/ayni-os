// Bounded Provider-to-owner delivery (C-PROV-006, C-CALL-012, ADR-0044).
// Unlike Host's single fixed acknowledgement, an attempt retains independent
// cancel/terminal receipts and coalesces progress. Reading never retires data:
// the owner acknowledges only after its domain transition can be committed.
#ifndef AYNI_CORE_KERNEL_COMPLETION_PROVIDER_MAILBOX_H
#define AYNI_CORE_KERNEL_COMPLETION_PROVIDER_MAILBOX_H

#include <ayni/ayni.h>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ayni::kernel {

// Transport classes, not Task outcomes; the consumer validates result semantics.
enum class ProviderMessageKind {
    completed,
    failed,
    cancelled,
    progress,
    cannot_cancel,
    cancel_unknown,
    abandoned,
};

// Untrusted, bounded message view; input data is copied before post returns.
struct ProviderMessage {
    ProviderMessageKind kind; // Chooses progress, cancel control or terminal lane.
    uint64_t sequence;        // Positive for progress, zero for all reliable receipts.
    ayni_status_t status;     // Classified scalar, interpreted by the domain consumer.
    ayni_reason_t reason;     // Classified scalar, never vendor text or credentials.
    ayni_bytes_v1 payload;    // Schema-owned bytes; cancel control has no payload.
};

// Identity of one reserved slot/attempt, scoped to this mailbox's lifetime.
// Never mutate or reconstruct it from the slot's current contents; a producer
// retains the exact value issued. This is not a security boundary.
struct ProviderReservation {
    uint32_t index;              // Pool-local slot, below its configured capacity.
    ayni_attempt_ref_v1 attempt; // Nonzero boot/request pair; globally assigned by the caller.
};

// A copied delivery's identity, valid independently of producer buffer changes.
struct ProviderReceipt {
    ProviderReservation reservation; // Exact slot incarnation, not a raw slot index.
    ProviderMessageKind kind;        // The lane/outcome selected by read_next.
    uint64_t sequence;               // Selected progress generation; zero for reliable receipts.
};

// Owns reserved delivery capacity, not call state, attempts or persistence.
// One owner calls reserve/read/acknowledge/withdraw; any non-ISR producer may
// post. All shared bytes use the Host ingress lock. No callback runs under it.
// Caller joins producers before destroying the mailbox; idle alone cannot prove
// that a misbehaving producer will never try another post.
class ProviderMailbox {
  public:
    struct Limits {
        uint32_t capacity;      // Positive reserved attempts, <= capacity_max; no dynamic growth.
        uint32_t payload_bytes; // Positive bytes per progress/terminal lane, <= payload_bytes_max.
    };
    static constexpr uint32_t capacity_max = 256;
    static constexpr uint32_t payload_bytes_max = 65536;
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    // Limits must satisfy the above bounds; storage is caller-owned for our lifetime,
    // aligned to storage_alignment and at least storage_size(limits) bytes long.
    static std::size_t storage_size(Limits limits);
    ProviderMailbox(void *storage, Limits limits, const ayni_host_v1 &host);
    ProviderMailbox(const ProviderMailbox &) = delete;
    ProviderMailbox &operator=(const ProviderMailbox &) = delete;

    // Owner: reserve before submit. Request IDs must strictly increase across
    // all reservations, including boot changes, so an old token never aliases.
    // Capacity or malformed identity errors preserve out and consume no identity.
    ayni_status_t reserve(ayni_attempt_ref_v1 attempt, ProviderReservation &out);
    // Any producer: valid progress can replace only progress; a cancel control
    // is accepted once. Terminal publication ends producer delivery: later
    // posts are rejected, while previously accepted cancel remains pending.
    // Older progress is acknowledged but ignored. All other errors preserve data.
    ayni_status_t post(ProviderReservation reservation, const ProviderMessage &message);
    // Owner: round-robin among ready attempts; within one, cancel precedes
    // terminal, then progress. Copies to caller buffer under lock and leaves
    // receipt pending. BUFFER_TOO_SMALL changes only required; other failures
    // change no output. payload may be null only when capacity is zero and must
    // not overlap the mailbox arena or the other outputs.
    ayni_status_t read_next(uint8_t *payload, uint32_t capacity, uint32_t &required,
                            ProviderMessage &out, ProviderReceipt &receipt);
    // A progress copy may be superseded while its owner applies it; newer
    // progress/terminal remains pending. Reliable receipts never get replaced.
    enum class Acknowledge { consumed, superseded, stale };
    Acknowledge acknowledge(const ProviderReceipt &receipt);
    // Owner: only when submit proved NOT_RETAINED and nothing was ever posted.
    // Refuses a contradictory producer post, preserving its evidence and slot.
    bool withdraw(ProviderReservation reservation);
    // Owner: every reserved attempt counts until terminal ack or legal withdraw.
    uint32_t in_flight() const { return in_flight_; }
    // Any thread: retained messages, independent of the owner's journal capacity.
    bool has_pending() const;

  private:
    // Scalars shared by result lanes; semantic validation belongs above transport.
    struct Result {
        ProviderMessageKind kind; // Selected lane/outcome, never an empty marker.
        ayni_status_t status;     // Copied classification; the domain validates its meaning.
        ayni_reason_t reason;     // Copied reason, with NONE permitted by the domain.
        uint32_t size;            // Payload bytes, <= payload_bytes_; zero is a valid payload.
    };
    // Keep acknowledged cancellation identity until terminal so duplicate control
    // cannot be accepted again. Separate stages do not overload payload length.
    struct Cancel {
        enum class Stage { pending, acknowledged };
        Result result; // Cannot-cancel/unknown, always without payload.
        Stage stage;   // One pending control is consumed once before terminal.
    };
    struct Attempt {
        ayni_attempt_ref_v1 identity;        // Exact nonzero incarnation of this reserved slot.
        uint64_t last_progress_sequence = 0; // Highest seen; zero means no progress ever posted.
        std::optional<Result> progress;      // Latest unapplied progress; empty after ack/terminal.
        std::optional<Cancel> cancel;        // Empty before the first accepted cancel response.
        std::optional<Result> terminal;      // Immutable once posted, released only by owner ack.
    };
    struct Slot {
        std::optional<Attempt> attempt; // Empty is free; all message state resets together.
    };
    // Helpers accessing slot state require the ingress lock. Message validation,
    // kind classification and address math read only immutable configuration.
    Attempt *find(ProviderReservation reservation);
    static const Result *pending(const Attempt &attempt);
    static bool is_cancel(ProviderMessageKind kind);
    static bool is_terminal(ProviderMessageKind kind);
    ayni_status_t check_message(const ProviderMessage &message) const;
    uint8_t *payload_of(uint32_t index, ProviderMessageKind kind) const;
    void release(uint32_t index);

    const ayni_host_v1 &host_; // Borrowed lock/wake ports, valid through producer join.
    Slot *slots_;              // Caller arena, capacity_ slots; owner reserves/releases under lock.
    uint8_t *payloads_;        // Two payload strides per slot: progress and terminal.
    uint32_t capacity_;        // Immutable positive slot count, bounded by capacity_max.
    uint32_t payload_bytes_;   // Immutable positive lane stride in bytes, bounded above.
    uint32_t in_flight_ = 0;   // Owner-written live reservations, at most capacity_.
    uint32_t cursor_ = 0;      // Next fair scan index, below capacity_.
    uint64_t last_request_id_ = 0; // Accepted reservation high-water mark, never reset.
};
} // namespace ayni::kernel
#endif
