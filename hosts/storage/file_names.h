// Reserved namespace shared by the Host's journal and outer value mechanisms.
#ifndef AYNI_HOST_FILE_NAMES_H
#define AYNI_HOST_FILE_NAMES_H
#include <cstring>
namespace ayni::hosts::storage {
inline constexpr char file_value_prefix[] = "value-";
inline bool is_file_value_name(const char *name) {
    return std::strncmp(name, file_value_prefix, sizeof(file_value_prefix) - 1) == 0;
}
} // namespace ayni::hosts::storage
#endif
