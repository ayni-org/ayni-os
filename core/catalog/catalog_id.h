// Catalog ids such as goal type ids (ayni.h AYNI_CATALOG_ID_MAX_BYTES): 1 to
// 32 bytes of lowercase ASCII letters, digits, '.', '_' and '-'. The narrow
// alphabet keeps ids comparable byte for byte and printable in logs without
// escaping.

#ifndef AYNI_CORE_CATALOG_CATALOG_ID_H
#define AYNI_CORE_CATALOG_CATALOG_ID_H

#include <ayni/ayni.h>

#include <array>
#include <cstdint>

namespace ayni::catalog {

class CatalogId {
  public:
    static constexpr uint32_t max_size = AYNI_CATALOG_ID_MAX_BYTES;

    // Whether the view is a catalog id; data may be null only when size is 0.
    static bool is_valid(ayni_string_v1 text);

    // Copies a valid id. Returns false, changing nothing, otherwise.
    bool assign(ayni_string_v1 text);

    bool equals(ayni_string_v1 text) const;
    // Copied ABI snapshot, zero padded; an empty ID keeps size zero.
    ayni_catalog_name_v1 snapshot() const;

    const char *data() const { return bytes_.data(); }
    uint32_t size() const { return size_; }

  private:
    std::array<char, max_size> bytes_{};
    uint32_t size_ = 0;
};

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_CATALOG_ID_H
