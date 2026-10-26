#ifndef AYNI_CLI_CONFIG_H
#define AYNI_CLI_CONFIG_H
#include <ayni/runtime/common/demo_profile.h>
#include <string>
namespace ayni::cli {
struct Options {
    std::string config_file, data_directory;
    runtime::ModelConfig model;
    bool help = false;
};
// Strict bounded key=value file. Errors never include configuration values.
bool read_options(int argc, char **argv, Options &out, std::string &error);
const char *usage();
} // namespace ayni::cli
#endif
