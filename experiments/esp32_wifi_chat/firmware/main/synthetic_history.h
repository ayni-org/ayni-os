#pragma once
#include <algorithm>
#include <array>
#include <ayni/model_messages.hpp>
namespace wifichat {
// One synthetic message generator shared by transport and real ToolLoop capacity checks.
struct SyntheticHistory {
    enum class Corpus { latin, han, escaped };
    static constexpr uint32_t message_units = 1024, frame_overhead = 32;
    static constexpr std::string_view han = "测";
    static size_t unit_bytes(Corpus corpus) { return corpus == Corpus::han ? han.size() : 1; }
    static uint64_t room(Corpus corpus, uint32_t units) {
        return uint64_t(units) * unit_bytes(corpus) +
               ((uint64_t(units) + message_units - 1) / message_units) * frame_overhead;
    }
    static void append(ayni::model::Writer &out, Corpus corpus, uint32_t units) {
        std::array<char, message_units * han.size()> prototype{};
        for (size_t at = 0; at < message_units; ++at) {
            if (corpus == Corpus::han) {
                std::memcpy(prototype.data() + at * han.size(), han.data(), han.size());
            } else {
                prototype[at] = corpus == Corpus::escaped ? '\n' : 'a';
            }
        }
        bool user = true;
        while (units) {
            const auto count = std::min(units, message_units);
            out.text(user ? ayni::model::Role::user : ayni::model::Role::assistant,
                     {prototype.data(), uint32_t(count * unit_bytes(corpus))});
            units -= count;
            user = !user;
        }
    }
};
} // namespace wifichat
