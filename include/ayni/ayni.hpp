// C++17 lifecycle helpers over the public C ABI. No exceptions or RTTI required.
#ifndef AYNI_AYNI_HPP
#define AYNI_AYNI_HPP
#include <ayni/ayni.h>
#include <new>
#include <optional>

namespace ayni {
// Initialize a versioned C descriptor without changing its ownership or defaults.
template <class T> T initialized() noexcept {
    static_assert(sizeof(T) <= UINT32_MAX, "C descriptor size must fit struct_size");
    T value{};
    value.struct_size = static_cast<uint32_t>(sizeof(T));
    return value;
}

// Owner-thread, nonthrowing progress hook. Context is borrowed only during drain.
struct Progress {
    void *context = nullptr;                    // Null is allowed when the callback needs no state.
    ayni_status_t (*advance)(void *) = nullptr; // Null: producers progress independently.
};
// Bounded, nonthrowing producer join/stop proof, borrowed through successful close.
struct Quiesce {
    void *context = nullptr; // Caller owns this context; moving Kernel does not move it.
    // Required. OK proves all producers returned from their last post, keeping registered
    // descriptors/contexts alive until destroy; BUSY/error preserves every borrow.
    ayni_status_t (*join)(void *) = nullptr;
};
// A copied submission outcome. Acceptance is not a durable receipt.
struct Submission {
    ayni_status_t status = AYNI_INVALID_STATE; // Classified C status, never inferred from phase.
    ayni_intent_result_v1 result = initialized<ayni_intent_result_v1>(); // Zero effect on refusal.
    ayni_error_v1 error = initialized<ayni_error_v1>(); // C diagnostic; NONE when no finer cause.
};

// Owns one Kernel handle and optionally its aligned arena. Host/Provider code,
// contexts and borrowed caller arenas remain caller-owned through successful close.
// All lifecycle methods and moves use the logical owner thread, outside callbacks.
// Hooks may progress their services but must not move or destroy this Kernel.
class Kernel {
  public:
    Kernel() noexcept = default; // Empty, no allocation or borrowed context.
    // No implicit asynchronous drain. If explicit close was skipped/refused, retain
    // the arena rather than release a live borrow. This does not extend caller-owned
    // Host/Provider/arena lifetimes. Call close and retain them until it succeeds.
    ~Kernel() noexcept {
        if (instance_ && close() != AYNI_OK) {
            instance_.reset();
        }
    }
    Kernel(const Kernel &) = delete;            // A handle/arena has exactly one lifecycle owner.
    Kernel &operator=(const Kernel &) = delete; // No copied ownership.
    // Moving transfers the complete borrow/arena record; the source becomes empty.
    Kernel(Kernel &&other) noexcept : instance_(other.instance_) { other.instance_.reset(); }
    Kernel &operator=(Kernel &&) = delete; // Never silently replace a live destination.

    // Allocate once with nothrow aligned new. C validates configuration and owns
    // no heap; allocation failure returns CAPACITY_EXCEEDED without a Kernel.
    ayni_status_t create(const ayni_config_v1 &config, const ayni_host_v1 &host,
                         Quiesce quiesce) noexcept {
        if (instance_) {
            return AYNI_INVALID_STATE;
        }
        if (!quiesce.join) {
            return AYNI_INVALID_ARGUMENT;
        }
        size_t bytes = 0, alignment = 0;
        const auto status = ayni_kernel_memory_requirements(&config, &bytes, &alignment);
        if (status != AYNI_OK) {
            return status;
        }
        void *memory = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
        if (!memory) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        return create_with(config, host, {memory, alignment, Ownership::allocated}, bytes, quiesce);
    }
    // Caller owns aligned storage through successful close; failure never frees it.
    // Alignment/size are checked once by the C create boundary.
    ayni_status_t create_in(const ayni_config_v1 &config, const ayni_host_v1 &host, void *arena,
                            size_t bytes, Quiesce quiesce) noexcept {
        if (instance_) {
            return AYNI_INVALID_STATE;
        }
        if (!quiesce.join) {
            return AYNI_INVALID_ARGUMENT;
        }
        return create_with(config, host, {arena, 0, Ownership::borrowed}, bytes, quiesce);
    }
    // Borrowed opaque C handle; null when empty, stable across wrapper moves.
    ayni_kernel_t *get() const noexcept { return instance_ ? instance_->kernel : nullptr; }
    // Start recovery through the C lifecycle; does not wait for storage.
    ayni_status_t start() noexcept { return get() ? ayni_start(get()) : AYNI_INVALID_STATE; }
    // One bounded C step. Failure preserves the caller's report.
    ayni_status_t step(uint32_t units, uint32_t max_us, ayni_step_report_v1 &out) noexcept {
        if (!get()) {
            return AYNI_INVALID_STATE;
        }
        auto report = initialized<ayni_step_report_v1>();
        const auto status = ayni_step(get(), units, max_us, &report);
        if (status == AYNI_OK) {
            out = report;
        }
        return status;
    }
    // Raw intent convenience: caller must pair kind with its specified C body.
    // The template retains the borrow, not a claim of kind/type safety.
    template <class Body>
    Submission submit_raw(ayni_intent_kind_t kind, uint64_t command, uint64_t session,
                          uint32_t revision, const Body &body) noexcept {
        Submission out;
        if (!get()) {
            out.error.status = out.status;
            return out;
        }
        const ayni_intent_v1 intent{sizeof(intent), kind, command, session, revision, 0, &body};
        out.status = ayni_submit_intent(get(), &intent, &out.result, &out.error);
        return out;
    }
    // Positive max_steps bounds owner steps, including caller progress; zero is
    // INVALID_ARGUMENT. OK means the
    // Kernel is CONFIGURING/STOPPED and can close; BUSY retains it for another call.
    // This does not prove producer threads joined. Hooks must be bounded/nonthrowing.
    ayni_status_t drain(uint32_t max_steps, Progress progress = {}) noexcept {
        if (!get()) {
            return AYNI_INVALID_STATE;
        }
        if (!max_steps) {
            return AYNI_INVALID_ARGUMENT;
        }
        ayni_lifecycle_t lifecycle = 0;
        auto status = ayni_get_lifecycle(get(), &lifecycle);
        if (status != AYNI_OK) {
            return status;
        }
        if (lifecycle == AYNI_LIFECYCLE_CONFIGURING || lifecycle == AYNI_LIFECYCLE_STOPPED) {
            return AYNI_OK;
        }
        if (lifecycle != AYNI_LIFECYCLE_DRAINING) {
            status = ayni_begin_shutdown(get());
            if (status != AYNI_OK) {
                return status;
            }
        }
        for (uint32_t at = 0; at < max_steps; ++at) {
            if (progress.advance) {
                status = progress.advance(progress.context);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            auto report = initialized<ayni_step_report_v1>();
            status = step(drain_units, drain_max_us, report);
            if (status != AYNI_OK) {
                return status;
            }
            status = ayni_get_lifecycle(get(), &lifecycle);
            if (status != AYNI_OK) {
                return status;
            }
            if (lifecycle == AYNI_LIFECYCLE_STOPPED) {
                return AYNI_OK;
            }
        }
        return AYNI_BUSY;
    }
    // Destroy only after drain and the configured producer join proof. A failure
    // retains the complete handle/arena/hook; success releases only owned storage.
    ayni_status_t close() noexcept {
        if (!instance_) {
            return AYNI_OK;
        }
        ayni_lifecycle_t lifecycle = 0;
        auto status = ayni_get_lifecycle(get(), &lifecycle);
        if (status != AYNI_OK) {
            return status;
        }
        if (lifecycle != AYNI_LIFECYCLE_CONFIGURING && lifecycle != AYNI_LIFECYCLE_STOPPED) {
            return AYNI_INVALID_STATE;
        }
        status = instance_->quiesce.join(instance_->quiesce.context);
        if (status != AYNI_OK) {
            return status;
        }
        status = ayni_destroy(get());
        if (status != AYNI_OK) {
            return status;
        }
        release(instance_->arena);
        instance_.reset();
        return AYNI_OK;
    }

  private:
    // Storage provenance; no boolean silently switches release algorithms.
    enum class Ownership { borrowed, allocated };
    // One arena owner record. Alignment is bytes; zero only for borrowed arenas.
    struct Arena {
        void *data;          // Non-null only after successful create; caller owns borrowed storage.
        size_t alignment;    // Allocated arena's positive C requirement, zero for borrowed.
        Ownership ownership; // Decides whether successful close may deallocate data.
    };
    // All fields become live/empty together; C owns the state inside the arena.
    struct Instance {
        ayni_kernel_t *kernel; // Non-null stable handle until successful destroy.
        Arena arena;           // Exactly the storage passed to create; never resized.
        Quiesce quiesce;       // Required callback/context borrowed through successful close.
    };
    std::optional<Instance> instance_;              // Empty before create/after close or move.
    static constexpr uint32_t drain_units = 64;     // Work items per drain step.
    static constexpr uint32_t drain_max_us = 10000; // Host microseconds per drain step.
    // Release only allocated storage; borrowed arenas remain with their caller.
    static void release(Arena arena) noexcept {
        if (arena.ownership == Ownership::allocated) {
            ::operator delete(arena.data, std::align_val_t{arena.alignment});
        }
    }
    // Both allocation paths share the one C construction boundary and cleanup rule.
    ayni_status_t create_with(const ayni_config_v1 &config, const ayni_host_v1 &host, Arena arena,
                              size_t bytes, Quiesce quiesce) noexcept {
        ayni_kernel_t *kernel = nullptr;
        const auto status = ayni_create(&config, &host, arena.data, bytes, &kernel);
        if (status != AYNI_OK) {
            release(arena);
            return status;
        }
        instance_ = Instance{kernel, arena, quiesce};
        return AYNI_OK;
    }
};
} // namespace ayni
#endif
