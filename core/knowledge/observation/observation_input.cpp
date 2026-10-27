#include "knowledge/observation/observation_input.h"
#include "catalog/abi_checks.h"
namespace {
bool valid_name(const ayni_catalog_name_v1 &name) {
    return ayni::catalog::CatalogId::is_valid({name.data, name.size});
}
} // namespace
extern "C" ayni_status_t ayni_validate_media_reference(const ayni_media_ref_v1 *reference) {
    if (reference == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &value = *reference;
    const auto shape =
        ayni::catalog::check_struct_size(value.struct_size, sizeof(value), sizeof(value));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (value.media_id == 0 || value.boot_epoch == 0 || value.owner_instance_id == 0 ||
        value.generation == 0 || value.byte_size == 0 || value.byte_size > AYNI_MEDIA_BYTES_MAX ||
        value.valid_until_us == 0 || value.valid_until_us == UINT64_MAX ||
        value.source_version == 0 || value.reserved != 0 || !valid_name(value.source_id) ||
        !valid_name(value.purpose) || value.content_type < AYNI_MEDIA_JPEG ||
        value.content_type > AYNI_MEDIA_MOCK_BYTES) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}
namespace ayni::knowledge {
ayni_status_t validate_observation_input(const ObservationInput &input) {
    if (input.source.size() == 0 || input.generation == 0 || input.sequence == 0 ||
        input.source_version == 0 || input.valid_for_us == 0 ||
        input.captured_at_us > UINT64_MAX - input.valid_for_us ||
        (input.quality != AYNI_OBSERVATION_GOOD && input.quality != AYNI_OBSERVATION_DEGRADED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (input.media) {
        const auto &media = *input.media;
        if (!input.source.equals({media.source_id.data, media.source_id.size}) ||
            input.source_version != media.source_version || input.generation != media.generation ||
            media.valid_until_us <= input.captured_at_us ||
            media.valid_until_us > input.captured_at_us + input.valid_for_us) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return AYNI_OK;
}
ayni_status_t check_observation_source(const ObservationInput &input,
                                       const catalog::ObservationSource &source) {
    if (input.source_version != source.version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    return input.valid_for_us > source.validity_max_us ||
                   (source.kind == AYNI_OBSERVATION_BOOL && input.value != 0 && input.value != 1)
               ? AYNI_INVALID_ARGUMENT
               : AYNI_OK;
}
bool after_boundary(uint64_t generation, uint64_t sequence, ObservationBoundary boundary) {
    return generation > boundary.generation ||
           (generation == boundary.generation && sequence > boundary.through);
}
} // namespace ayni::knowledge
