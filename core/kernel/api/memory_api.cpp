// Owner-only management queries. Strategy access is bound to a decision lease separately.
#include "catalog/abi_checks.h"
#include "foundation/utf8.h"
#include "kernel/api/api_gate.h"
#include "kernel/service/memory/memory_access.h"
#include <cstring>
namespace {
ayni_status_t owner_access(const ayni_kernel_t &kernel, uint64_t session) {
    const auto gate = ayni::kernel::admit(kernel, ayni::kernel::gate::running);
    if (gate != AYNI_OK) {
        return gate;
    }
    return ayni::kernel::memory_management(kernel.model(), session, kernel.durable_seq())
               ? AYNI_OK
               : AYNI_DENIED;
}
} // namespace
extern "C" {
// A bounded stable-ID page checks visibility before content matching; no partial failure output.
ayni_status_t ayni_search_memory(const ayni_kernel_t *kernel, uint64_t session_id,
                                 const ayni_memory_search_v1 *query, ayni_memory_info_v1 *out,
                                 uint32_t capacity, uint32_t *out_count) {
    if (kernel == nullptr || query == nullptr || out_count == nullptr ||
        (capacity != 0 && out == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto form = ayni::kernel::memory_search_form(*query);
    if (form != AYNI_OK) {
        return form;
    }
    const auto access_status = owner_access(*kernel, session_id);
    if (access_status != AYNI_OK) {
        return access_status;
    }
    const auto &model = kernel->model();
    const auto space = static_cast<ayni::knowledge::MemoryNamespace>(query->name_space);
    if (!ayni::kernel::memory_namespace_exists(model, space, query->namespace_id)) {
        return AYNI_NOT_FOUND;
    }
    const ayni::kernel::MemoryAccess access{model, kernel->now_us(), kernel->durable_seq()};
    uint32_t count = 0;
    uint64_t after = query->after_id;
    while (count < capacity) {
        uint32_t selected = model.memories.size();
        for (uint32_t i = 0; i < model.memories.size(); ++i) {
            const auto &r = model.memories.at(i);
            if (r.id > after && r.space == space && r.namespace_id == query->namespace_id &&
                (selected == model.memories.size() || r.id < model.memories.at(selected).id)) {
                selected = i;
            }
        }
        if (selected == model.memories.size()) {
            break;
        }
        after = model.memories.at(selected).id;
        const auto info = ayni::kernel::memory_info(access, selected);
        if (info.usable != 0 && model.memories.matches(selected, query->keyword, query->tag)) {
            out[count++] = info;
        }
    }
    *out_count = count;
    return AYNI_OK;
}
// Metadata and body share one visibility decision. Deleted/invalid derivations expose no text.
ayni_status_t ayni_get_memory(const ayni_kernel_t *kernel, uint64_t session_id, uint64_t memory_id,
                              ayni_memory_info_v1 *out_info, char *body, uint32_t capacity,
                              uint32_t *out_bytes) {
    if (kernel == nullptr || out_info == nullptr || out_bytes == nullptr || memory_id == 0 ||
        (body == nullptr && capacity != 0)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                        sizeof(*out_info));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto access_status = owner_access(*kernel, session_id);
    if (access_status != AYNI_OK) {
        return access_status;
    }
    const auto &model = kernel->model();
    uint32_t index = 0;
    if (!model.memories.find(memory_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto info =
        ayni::kernel::memory_info({model, kernel->now_us(), kernel->durable_seq()}, index);
    if (body != nullptr) {
        if (info.usable == 0) {
            return AYNI_EXPIRED;
        }
        if (capacity < info.body_bytes) {
            *out_bytes = info.body_bytes;
            return AYNI_BUFFER_TOO_SMALL;
        }
        const auto text = model.memories.body(index);
        std::memcpy(body, text.data, text.size);
    }
    *out_info = info;
    *out_bytes = body == nullptr ? 0 : info.body_bytes;
    return AYNI_OK;
}
}
