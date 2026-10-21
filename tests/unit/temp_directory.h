// A new directory under GoogleTest's temporary directory, removed with
// everything in it when the object goes.

#ifndef AYNI_TESTS_UNIT_TEMP_DIRECTORY_H
#define AYNI_TESTS_UNIT_TEMP_DIRECTORY_H

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace ayni::test {

class TempDirectory {
  public:
    TempDirectory() {
        const std::string pattern = ::testing::TempDir() + "ayni-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        if (::mkdtemp(name.data()) == nullptr) {
            ADD_FAILURE() << "mkdtemp failed for " << pattern;
            return;
        }
        path_ = name.data();
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    const std::string &path() const { return path_; }
    std::string file(const std::string &name) const { return path_ + "/" + name; }

  private:
    std::string path_;
};

} // namespace ayni::test

#endif // AYNI_TESTS_UNIT_TEMP_DIRECTORY_H
