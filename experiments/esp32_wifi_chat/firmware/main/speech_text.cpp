#include "speech_text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <md4c.h>
#include <new>
extern "C" {
#include <entity.h>
}
namespace wifichat {
namespace {
bool whitespace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
// MD4C callbacks write into one caller-budgeted buffer. Separators are delayed so nested
// blocks collapse cleanly and no leading/trailing delimiter needs another allocation.
struct Writer {
    enum class Separator { none, space, cell, line };
    char *bytes;
    size_t capacity, size = 0;
    ayni_status_t status = AYNI_OK; // The output owner retains overflow even if parsing aborts.
    Separator pending = Separator::none;
    void separate(Separator next) {
        if (next > pending) {
            pending = next;
        }
    }
    bool raw(std::string_view value) {
        if (value.size() > capacity - size) {
            status = AYNI_CAPACITY_EXCEEDED;
            return false;
        }
        std::copy(value.begin(), value.end(), bytes + size);
        size += value.size();
        return true;
    }
    bool append(std::string_view value, Separator line_break = Separator::space) {
        for (char c : value) {
            if (whitespace(c)) {
                separate(c == '\n' ? line_break : Separator::space);
                continue;
            }
            if (size && pending != Separator::none) {
                const auto separator = pending == Separator::line   ? "\n"
                                       : pending == Separator::cell ? "；"
                                                                    : " ";
                if (!raw(separator)) {
                    return false;
                }
            }
            pending = Separator::none;
            if (!raw({&c, 1})) {
                return false;
            }
        }
        return true;
    }
    bool point(uint32_t value) {
        if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            value = 0xfffd;
        }
        if (value == 0xa0) {
            return append(" ");
        }
        std::array<char, 4> encoded{};
        size_t encoded_size = 0;
        if (value < 0x80) {
            encoded[encoded_size++] = char(value);
        } else if (value < 0x800) {
            encoded[encoded_size++] = char(0xc0 | (value >> 6));
            encoded[encoded_size++] = char(0x80 | (value & 0x3f));
        } else if (value < 0x10000) {
            encoded[encoded_size++] = char(0xe0 | (value >> 12));
            encoded[encoded_size++] = char(0x80 | ((value >> 6) & 0x3f));
            encoded[encoded_size++] = char(0x80 | (value & 0x3f));
        } else {
            encoded[encoded_size++] = char(0xf0 | (value >> 18));
            encoded[encoded_size++] = char(0x80 | ((value >> 12) & 0x3f));
            encoded[encoded_size++] = char(0x80 | ((value >> 6) & 0x3f));
            encoded[encoded_size++] = char(0x80 | (value & 0x3f));
        }
        return append({encoded.data(), encoded_size});
    }
    bool entity(std::string_view value) {
        if (value.size() > 3 && value[1] == '#') {
            value.remove_prefix(2);
            value.remove_suffix(1);
            const bool hex = value.front() == 'x' || value.front() == 'X';
            if (hex) {
                value.remove_prefix(1);
            }
            uint32_t code = 0;
            const auto read =
                std::from_chars(value.data(), value.data() + value.size(), code, hex ? 16 : 10);
            return point(
                read.ec == std::errc{} && read.ptr == value.data() + value.size() ? code : 0xfffd);
        }
        const auto *found = entity_lookup(value.data(), value.size());
        return found ? point(found->codepoints[0]) &&
                           (!found->codepoints[1] || point(found->codepoints[1]))
                     : append(value); // Unknown named entities remain text, not silently discarded.
    }
};
int enter_block(MD_BLOCKTYPE type, void *detail, void *context) {
    auto &out = *static_cast<Writer *>(context);
    if (type != MD_BLOCK_DOC && type != MD_BLOCK_TH && type != MD_BLOCK_TD) {
        out.separate(Writer::Separator::line);
    }
    if (type == MD_BLOCK_LI) {
        const auto &item = *static_cast<MD_BLOCK_LI_DETAIL *>(detail);
        if (item.is_task) {
            return out.append(item.task_mark == ' ' ? "未完成：" : "已完成：") ? 0
                                                                               : int(out.status);
        }
    }
    return 0;
}
int leave_block(MD_BLOCKTYPE type, void *, void *context) {
    auto &out = *static_cast<Writer *>(context);
    out.separate(type == MD_BLOCK_TH || type == MD_BLOCK_TD ? Writer::Separator::cell
                                                            : Writer::Separator::line);
    return 0;
}
int span(MD_SPANTYPE, void *, void *) {
    return 0; // Keep span text/alt text; formatting, link destinations and titles are not spoken.
}
int text(MD_TEXTTYPE type, const char *bytes, MD_SIZE size, void *context) {
    auto &out = *static_cast<Writer *>(context);
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        out.separate(type == MD_TEXT_BR ? Writer::Separator::line : Writer::Separator::space);
        return 0;
    }
    const std::string_view value(bytes, size);
    return (type == MD_TEXT_ENTITY ? out.entity(value)
            : type == MD_TEXT_NULLCHAR
                ? out.point(0xfffd)
                : out.append(value, type == MD_TEXT_CODE ? Writer::Separator::line
                                                         : Writer::Separator::space))
               ? 0
               : int(out.status);
}
size_t width(uint8_t first) {
    return first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
}
uint32_t point(std::string_view text, size_t at, size_t length) {
    uint32_t value = uint8_t(text[at]) & (length == 1 ? 0x7fU : (1U << (7 - length)) - 1);
    for (size_t byte = 1; byte < length; ++byte) {
        value = (value << 6) | (uint8_t(text[at + byte]) & 0x3fU);
    }
    return value;
}
bool closing(uint32_t value) {
    return value == '"' || value == '\'' || value == ')' || value == ']' || value == '}' ||
           value == 0x2019 || value == 0x201d || value == 0x300d || value == 0x300f ||
           value == 0xff09 || value == 0x3011;
}
bool period_ends_sentence(std::string_view text, size_t at) {
    while (at < text.size()) {
        const auto length = width(uint8_t(text[at]));
        if (!closing(point(text, at, length))) {
            return whitespace(text[at]);
        }
        at += length;
    }
    return true;
}
} // namespace
ayni_status_t SpeechText::render(std::string_view markdown, size_t maximum) {
    if (!maximum || maximum > UINT32_MAX || markdown.size() > UINT32_MAX ||
        markdown.find('\0') != markdown.npos ||
        ayni_validate_utf8({markdown.data(), uint32_t(markdown.size())}) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[maximum]);
    if (!buffer) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Writer out{buffer.get(), maximum};
    MD_PARSER parser{};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_NOHTML;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = parser.leave_span = span;
    parser.text = text;
    // Markdown punctuation is handled by the pinned parser. Literal code, escaped symbols,
    // negative numbers, formulas and raw HTML text retain their meaning (ADR-0101).
    const auto parsed = md_parse(markdown.data(), MD_SIZE(markdown.size()), &parser, &out);
    if (parsed != 0 || out.status != AYNI_OK) {
        return AYNI_CAPACITY_EXCEEDED; // Parser allocation or the explicit output budget failed.
    }
    bytes_ = std::move(buffer);
    size_ = out.size;
    return AYNI_OK;
}
size_t speech_chunk_bytes(std::string_view text, size_t maximum) {
    size_t limit = std::min(text.size(), maximum);
    while (limit && limit < text.size() && (uint8_t(text[limit]) & 0xc0) == 0x80) {
        --limit;
    }
    if (limit == text.size()) {
        return limit;
    }
    size_t sentence = 0, word = 0;
    for (size_t at = 0; at < limit;) {
        const auto length = width(uint8_t(text[at]));
        const auto code = point(text, at, length);
        const auto end = at + length;
        const bool space = length == 1 && whitespace(text[at]);
        const bool period = code == '.' && period_ends_sentence(text, end);
        if (code == 0x3002 || code == 0xff01 || code == 0xff1f || code == '!' || code == '?' ||
            code == '\n' || period || (sentence && sentence == at && (space || closing(code)))) {
            sentence = end;
        }
        if (space || code == ',' || code == ';' || code == 0xff0c || code == 0xff1b ||
            code == 0x3001) {
            word = end;
        }
        at = end;
    }
    return sentence ? sentence : word ? word : limit;
}
} // namespace wifichat
