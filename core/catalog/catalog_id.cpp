#include "catalog/catalog_id.h"

#include <cstring>

namespace ayni::catalog {
namespace {

bool is_id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

} // namespace

bool CatalogId::is_valid(ayni_string_v1 text) {
    if (text.data == nullptr || text.size == 0U || text.size > max_size) {
        return false;
    }
    for (uint32_t index = 0; index < text.size; ++index) {
        if (!is_id_char(text.data[index])) {
            return false;
        }
    }
    return true;
}

bool CatalogId::assign(ayni_string_v1 text) {
    if (!is_valid(text)) {
        return false;
    }
    std::memcpy(bytes_.data(), text.data, text.size);
    size_ = text.size;
    return true;
}

bool CatalogId::equals(ayni_string_v1 text) const {
    if (text.size != size_) {
        return false;
    }
    return size_ == 0U ||
           (text.data != nullptr && std::memcmp(bytes_.data(), text.data, size_) == 0);
}

} // namespace ayni::catalog
