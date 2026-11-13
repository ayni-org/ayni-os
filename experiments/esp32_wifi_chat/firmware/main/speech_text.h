#pragma once
#include <ayni/ayni.h>
#include <memory>
#include <string_view>
namespace wifichat {
// Owned, bounded presentation text. Rendering never edits conversation text or a model tool call.
class SpeechText {
  public:
    // Owner thread only. Replaces the text on success, including an empty formatting-only reply.
    // Refuses invalid UTF-8/NUL, allocation failure or output exceeding maximum bytes.
    ayni_status_t render(std::string_view markdown, size_t maximum);
    std::string_view view() const {
        return bytes_ ? std::string_view(bytes_.get(), size_) : std::string_view{};
    }
    void clear() {
        bytes_.reset();
        size_ = 0;
    }

  private:
    std::unique_ptr<char[]> bytes_; // Exclusive output allocation; no parser or Provider borrow.
    size_t size_ = 0; // UTF-8 bytes, zero iff no readable text; bounded by render's maximum.
};
// Prefer sentence/paragraph boundaries, then word/clause boundaries, then a whole UTF-8 prefix.
// Input is already validated presentation text; the returned byte count never exceeds maximum.
size_t speech_chunk_bytes(std::string_view text, size_t maximum);
} // namespace wifichat
