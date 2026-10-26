#ifndef AYNI_CLI_FORMAT_H
#define AYNI_CLI_FORMAT_H
#include "commands.h"
namespace ayni::cli {
std::string format(const runtime::Reply &reply, Action action, runtime::Operation operation);
std::string format(const runtime::Notice &notice);
} // namespace ayni::cli
#endif
