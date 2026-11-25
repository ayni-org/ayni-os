// Trusted source/publication boundary; catalog and knowledge own all facts.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include <ayni/observations.h>

namespace {
// Decode the borrowed ABI once; catalog registration receives only its typed value.
ayni_status_t decode_source(const ayni_observation_source_v1 &input,
                            ayni::catalog::ObservationSource &out) {
    const auto shape =
        ayni::catalog::check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    ayni::catalog::ObservationSource value{};
    if (!value.id.assign(input.source_id) || !value.unit.assign(input.unit)) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    value.kind = input.kind;
    value.origin = input.origin;
    value.validity_max_us = input.validity_max_us;
    const auto status = ayni::catalog::validate_source(value);
    if (status == AYNI_OK) {
        out = value;
    }
    return status;
}
// Copy transient C views once; work transports only the validated value.
ayni_status_t decode_sample(const ayni_observation_v1 &raw, const ayni_media_ref_v1 *media,
                            ayni::knowledge::ObservationInput &out) {
    const auto shape = ayni::catalog::check_struct_size(raw.struct_size, sizeof(raw), sizeof(raw));
    if (shape != AYNI_OK) {
        return shape;
    }
    ayni::knowledge::ObservationInput value{};
    if (raw.reserved != 0 || !value.source.assign(raw.source_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.source_version = raw.source_version;
    value.generation = raw.generation;
    value.sequence = raw.sequence;
    value.captured_at_us = raw.captured_at_us;
    value.valid_for_us = raw.valid_for_us;
    value.quality = raw.quality;
    value.value = raw.value;
    if (media != nullptr) {
        // Validate before copying: a rejected short ABI object need not contain
        // the full current layout. Knowledge checks only sample/media pairing.
        const auto status = ayni_validate_media_reference(media);
        if (status != AYNI_OK) {
            return status;
        }
        value.media = *media;
    }
    const auto status = ayni::knowledge::validate_observation_input(value);
    if (status == AYNI_OK) {
        out = value;
    }
    return status;
}
} // namespace

extern "C" {
ayni_status_t ayni_register_observation_source(ayni_kernel_t *kernel,
                                               const ayni_observation_source_v1 *source) {
    if (kernel == nullptr || source == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::ObservationSource decoded{};
    const auto status = decode_source(*source, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(decoded);
}
ayni_status_t ayni_publish_observation(ayni_kernel_t *kernel,
                                       const ayni_observation_v1 *observation) {
    if (kernel == nullptr || observation == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    ayni::knowledge::ObservationInput decoded{};
    const auto status = decode_sample(*observation, nullptr, decoded);
    return status == AYNI_OK ? kernel->publish_observation(decoded) : status;
}
// This worker boundary deliberately bypasses owner-thread gates; the locked
// ingress boot gate decides acceptance without racing the owner lifecycle.
ayni_status_t ayni_post_observation(ayni_kernel_t *kernel, const ayni_observation_post_v1 *post) {
    if (kernel == nullptr || post == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(post->struct_size, sizeof(*post), sizeof(*post));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (post->reserved != 0 || post->boot_epoch == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::knowledge::ObservationInput decoded{};
    const auto status = decode_sample(post->sample, post->media, decoded);
    return status == AYNI_OK ? kernel->post_observation(post->boot_epoch, decoded) : status;
}
// Lock-free any-thread latest-wins sensor publication into triple-buffered slots (ADR-0112).
ayni_status_t ayni_publish_observation_latest(ayni_kernel_t *kernel,
                                              const ayni_observation_post_v1 *post) {
    if (kernel == nullptr || post == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(post->struct_size, sizeof(*post), sizeof(*post));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (post->reserved != 0 || post->boot_epoch == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::knowledge::ObservationInput decoded{};
    const auto status = decode_sample(post->sample, post->media, decoded);
    return status == AYNI_OK ? kernel->publish_observation_latest(post->boot_epoch, decoded)
                             : status;
}
ayni_status_t ayni_get_observation_slots(const ayni_kernel_t *kernel,
                                         ayni_observation_slot_info_v1 *out) {
    if (kernel == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    *out = kernel->observation_slots_view().info();
    return AYNI_OK;
}
// Gaps share admission identity, but have source-reserved retention.
ayni_status_t ayni_report_observation_gap(ayni_kernel_t *kernel,
                                          const ayni_observation_gap_v1 *gap) {
    if (kernel == nullptr || gap == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(gap->struct_size, sizeof(*gap), sizeof(*gap));
    if (shape != AYNI_OK) {
        return shape;
    }
    ayni::catalog::CatalogId source;
    if (gap->boot_epoch == 0 || gap->generation == 0 || gap->source_version == 0 ||
        !source.assign(gap->source_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return kernel->report_observation_gap(gap->boot_epoch, source, gap->source_version,
                                          {gap->generation, gap->through_sequence});
}
// Opaque immutable target plus original opening avoids reading any owner state
// on a Provider worker. Reuse the Host sample decoder and transport validator.
ayni_status_t ayni_subscription_post_observation(ayni_subscription_sink_t *sink,
                                                 const ayni_subscription_observation_v1 *post) {
    if (!sink || !post || !sink->pool || !sink->pool->owns(sink)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(post->struct_size, sizeof(*post), sizeof(*post));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (post->reserved != 0 || post->subscription.subscription_id == 0 ||
        post->subscription.boot_epoch == 0 || post->subscription.generation == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::knowledge::ObservationInput value{};
    const auto status = decode_sample(post->sample, post->media, value);
    return status == AYNI_OK ? sink->pool->post_subscription(sink->index, post->subscription, value)
                             : status;
}
ayni_status_t ayni_subscription_report_gap(ayni_subscription_sink_t *sink,
                                           ayni_subscription_ref_v1 reference, uint64_t through) {
    if (!sink || !sink->pool || !sink->pool->owns(sink) || reference.subscription_id == 0 ||
        reference.boot_epoch == 0 || reference.generation == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    return sink->pool->subscription_gap(sink->index, reference, through);
}
ayni_status_t ayni_get_observation_ingress(const ayni_kernel_t *kernel,
                                           ayni_observation_ingress_info_v1 *out) {
    if (kernel == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    *out = kernel->observation_ingress_view().info();
    return AYNI_OK;
}
ayni_status_t ayni_get_observation(const ayni_kernel_t *kernel, ayni_string_v1 source_id,
                                   ayni_observation_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr ||
        !ayni::catalog::CatalogId::is_valid(source_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                        sizeof(*out_info));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    const auto status = kernel->observation_view().info(source_id, kernel->now_us(), *out_info);
    if (status == AYNI_OK && kernel->state() != AYNI_LIFECYCLE_RUNNING) {
        out_info->usable = 0; // A fenced/recovering Kernel cannot consume old physical context.
    }
    return status;
}
}
