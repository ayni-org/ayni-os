// Synchronous body-store boundary: returned-reference verification, persistence requests,
// and bounded operation-local immutable read reuse. No conversation encoding or Core state.
#include "support.h"
namespace ayni::strategies::detail {
bool valid_ref(const BodyRef &ref) {
    return ref.reserved == 0U && (ref.id != 0U || (ref.digest == 0U && ref.size == 0U));
}
ayni_status_t put_body(const ayni_body_store_v1 &bodies, uint64_t scope, ayni_string_v1 text,
                       BodyRef &out) {
    if (!bodies.write || (text.size && !text.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(text.data), text.size};
    BodyRef stored{};
    const auto status = bodies.write(bodies.context, scope, bytes, &stored);
    if (status != AYNI_OK) {
        return status;
    }
    // A store that answers with another body could make Core execute what was not authorized.
    if (ayni_body_verify(&stored, bytes) != AYNI_OK) {
        return AYNI_STORAGE_FAILED;
    }
    out = stored;
    return AYNI_OK;
}
ayni_status_t save_body(const ayni_body_store_v1 &bodies, uint64_t scope, ayni_string_v1 text,
                        BodyRef &out) {
    auto status = put_body(bodies, scope, text, out);
    if (status == AYNI_OK && bodies.confirm && bodies.write == bodies.stage) {
        // C-HVAL-009: enqueue a new conversation body's persistence at creation,
        // not by rewalking all old history at every checkpoint. This is no durable ACK.
        status = bodies.confirm(bodies.context, &out);
        return status == AYNI_NOT_READY ? AYNI_OK : status;
    }
    return status;
}
ayni_status_t get_body(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out) {
    if (!bodies.read || ref.id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::string bytes(ref.size, '\0');
    const auto status =
        bodies.read(bodies.context, &ref, reinterpret_cast<uint8_t *>(bytes.data()), ref.size);
    if (status != AYNI_OK) {
        return status;
    }
    if (ayni_body_verify(&ref, {reinterpret_cast<const uint8_t *>(bytes.data()), ref.size}) !=
        AYNI_OK) {
        return AYNI_STORAGE_FAILED;
    }
    out = std::move(bytes);
    return AYNI_OK;
}
ayni_status_t BodyReader::read(const BodyRef &ref, Body &out) {
    const auto found = loaded_.find(ref.id);
    if (found != loaded_.end()) {
        const auto &previous = found->second.reference;
        if (previous.digest != ref.digest || previous.size != ref.size ||
            previous.reserved != ref.reserved) {
            return AYNI_STORAGE_FAILED;
        }
        out = found->second.bytes;
        return AYNI_OK;
    }
    std::string bytes;
    const auto status = get_body(store_, ref, bytes);
    if (status != AYNI_OK) {
        return status;
    }
    auto owned = std::make_shared<const std::string>(std::move(bytes));
    if (loaded_.size() < cache_entries && owned->capacity() <= cache_bytes - retained_) {
        retained_ += owned->capacity();
        loaded_.emplace(ref.id, Cached{ref, owned});
    }
    out = std::move(owned);
    return AYNI_OK;
}
ayni_status_t BodyReader::copy(const BodyRef &ref, std::string &out) {
    if (ref.id == 0U) {
        out.clear();
        return AYNI_OK;
    }
    // Avoid a second large allocation just to copy a streamed field into its final owner.
    if (loaded_.count(ref.id) == 0U &&
        (loaded_.size() == cache_entries || ref.size > cache_bytes - retained_)) {
        return get_body(store_, ref, out);
    }
    Body bytes;
    const auto status = read(ref, bytes);
    if (status == AYNI_OK) {
        out = *bytes;
    }
    return status;
}
ayni_status_t BodyReader::copy_text(const BodyRef &ref, std::string &out) {
    const auto status = copy(ref, out);
    if (status != AYNI_OK) {
        return status;
    }
    const auto found = loaded_.find(ref.id);
    if ((found == loaded_.end() || !found->second.text_checked) && !valid_text(view(out))) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (found != loaded_.end()) {
        found->second.text_checked = true;
    }
    return AYNI_OK;
}
ayni_status_t BodyReader::prepare(const std::vector<BodyRef> &refs) {
    bool pending = false;
    for (const auto &ref : refs) {
        Body bytes;
        const auto status = read(ref, bytes);
        if (status == AYNI_NOT_READY) {
            pending = true;
        } else if (status != AYNI_OK) {
            return status;
        }
    }
    return pending ? AYNI_NOT_READY : AYNI_OK;
}
} // namespace ayni::strategies::detail
