#ifndef AYNI_CLI_CONSOLE_H
#define AYNI_CLI_CONSOLE_H
#include <ayni/ayni.h>
#include <memory>
#include <string>
namespace ayni::cli {
// One physical writer. Owner/input threads only post; the Provider worker may
// wait for its own actual flush receipt. Failed drain retains the entire context.
class Console {
  public:
    Console();
    ~Console();
    Console(const Console &) = delete;
    Console &operator=(const Console &) = delete;
    ayni_status_t post(const std::string &);
    ayni_status_t present(const std::string &);
    ayni_status_t status() const;
    ayni_status_t close(); // Finite drain/join after the application releases its channel borrow.
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::cli
#endif
