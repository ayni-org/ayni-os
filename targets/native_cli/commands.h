#ifndef AYNI_CLI_COMMANDS_H
#define AYNI_CLI_COMMANDS_H
#include <ayni/runtime/native_node/assistant.h>
#include <string>
namespace ayni::cli {
enum class Action { execute, wait, quit, help };
struct Request {
    Action action = Action::execute;
    runtime::Command command;
};
bool parse(const std::string &line, Request &out);
const char *help();
} // namespace ayni::cli
#endif
