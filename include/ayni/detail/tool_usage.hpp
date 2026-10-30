// Shared bounded descriptive value. Offsets make copies independent of their
// destination address; views borrow only the receiving owner (ADR-0085).
#ifndef AYNI_DETAIL_TOOL_USAGE_HPP
#define AYNI_DETAIL_TOOL_USAGE_HPP
#include <array>
#include <ayni/tool_usage.h>
#include <cstring>

namespace ayni::detail {
// Owns a copied, bounded exact-pair description; views are produced without allocation.
class ToolUsage {
  public:
    // Admit exact identity/fields and UTF-8 once, then copy without retaining callers.
    bool assign(const ayni_tool_usage_v1 &source, const ayni_tool_v1 &tool) {
        const auto same = [](ayni_string_v1 a, ayni_string_v1 b) {
            return a.size == b.size && a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0;
        };
        if (source.struct_size != sizeof(source) || source.reserved ||
            source.binding_version != tool.binding_version ||
            source.capability_version != tool.capability_version ||
            !same(source.binding_id, tool.binding_id) ||
            !same(source.capability_id, tool.capability_id) ||
            source.field_count != tool.field_count || tool.field_count > fields_.size() ||
            (source.field_count ? source.fields == nullptr : source.fields != nullptr)) {
            return false;
        }
        ToolUsage candidate;
        candidate.binding_version_ = source.binding_version;
        candidate.capability_version_ = source.capability_version;
        const std::array<ayni_string_v1, text_count> strings{
            source.binding_id, source.capability_id, source.purpose,
            source.text_usage, source.result_usage,  source.examples};
        for (uint32_t at = 0; at < text_count; ++at) {
            if (!candidate.append(strings[at], AYNI_TOOL_USAGE_MAX_BYTES, candidate.text_[at])) {
                return false;
            }
        }
        candidate.count_ = source.field_count;
        for (uint32_t at = 0; at < tool.field_count; ++at) {
            const ayni_tool_field_usage_v1 *found = nullptr;
            for (uint32_t index = 0; index < source.field_count; ++index) {
                const auto &field = source.fields[index];
                if (field.struct_size != sizeof(field)) {
                    return false;
                }
                if (field.field_id == tool.fields[at].field_id) {
                    if (found) {
                        return false;
                    }
                    found = &field;
                }
            }
            if (!found || !candidate.append(found->description, AYNI_TOOL_FIELD_USAGE_MAX_BYTES,
                                            candidate.fields_[at].text)) {
                return false;
            }
            candidate.fields_[at].id = found->field_id;
        }
        *this = candidate;
        return true;
    }
    // Already admitted strings, with fields rebased into caller-owned descriptors.
    void read(ayni_tool_usage_v1 &out,
              std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> &fields) const {
        out = {sizeof(out),
               binding_version_,
               string(text_[binding_text]),
               capability_version_,
               string(text_[capability_text]),
               string(text_[purpose_text]),
               string(text_[input_text]),
               string(text_[result_text]),
               string(text_[examples_text]),
               count_,
               0,
               count_ ? fields.data() : nullptr};
        for (uint32_t at = 0; at < count_; ++at) {
            fields[at] = {sizeof(fields[at]), fields_[at].id, string(fields_[at].text)};
        }
    }

  private:
    // One text range owned by bytes_; unused entries have size zero.
    struct Slice {
        uint32_t offset = 0; // Byte offset into bytes_, zero also denotes the first string.
        uint32_t size = 0;   // UTF-8 bytes, zero only in an unused range.
    };
    // One authoritative field and its explanation, never a type or authority rule.
    struct Field {
        uint32_t id = 0; // Positive domain field ID in the used prefix; zero otherwise.
        Slice text;      // Explanation range within the same owned byte region.
    };
    enum TextIndex : uint32_t {
        binding_text = 0,
        capability_text,
        purpose_text,
        input_text,
        result_text,
        examples_text,
        text_count
    }; // Canonical descriptive field order.
    bool append(ayni_string_v1 value, uint32_t limit, Slice &slice) {
        if (!value.size || value.size > limit || value.size > bytes_.size() - used_ ||
            ayni_validate_utf8(value) != AYNI_OK) {
            return false;
        }
        slice = {used_, value.size};
        std::memcpy(bytes_.data() + used_, value.data, value.size);
        used_ += value.size;
        return true;
    }
    ayni_string_v1 string(Slice slice) const { return {bytes_.data() + slice.offset, slice.size}; }
    uint32_t binding_version_ = 0;    // Exact positive Binding version after assign; zero before.
    uint32_t capability_version_ = 0; // Exact positive Capability version, paired with binding.
    uint32_t used_ = 0;  // Occupied bytes, bounded by TOOL_USAGE_MAX_BYTES; zero before assign.
    uint32_t count_ = 0; // Used field prefix; zero is an explicitly empty domain.
    std::array<Slice, text_count> text_{}; // Identity and main-description ranges in bytes_.
    std::array<Field, AYNI_AUTHORITY_FIELD_COUNT_MAX>
        fields_{}; // Exact domain order in [0,count_).
    std::array<char, AYNI_TOOL_USAGE_MAX_BYTES>
        bytes_{}; // Owns [0,used_) UTF-8; no caller pointers.
};
} // namespace ayni::detail
#endif
