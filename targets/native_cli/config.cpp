#include "config.h"
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
namespace ayni::cli {
namespace {
constexpr size_t config_bytes_max = 16384;
std::string environment(const char *name) {
    const auto *value = std::getenv(name);
    return value ? value : "";
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
bool minute(const std::string &value, uint32_t &out) {
    if (value.size() != 5 || value[2] != ':') {
        return false;
    }
    uint32_t hour = 0, part = 0;
    const auto h = std::from_chars(value.data(), value.data() + 2, hour);
    const auto m = std::from_chars(value.data() + 3, value.data() + 5, part);
    if (h.ec != std::errc{} || h.ptr != value.data() + 2 || m.ec != std::errc{} ||
        m.ptr != value.data() + 5 || hour >= 24 || part >= 60) {
        return false;
    }
    out = hour * 60 + part;
    return true;
}
bool load(const std::string &path, bool required, Options &options, std::string &error) {
    auto &model = options.model;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec || (!exists && required)) {
        error = "无法读取指定配置文件。";
        return false;
    }
    if (!exists) {
        return true;
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > config_bytes_max) {
        error = "配置文件无效或超过 16 KiB。";
        return false;
    }
    std::ifstream file(path);
    if (!file) {
        error = "无法读取配置文件。";
        return false;
    }
    // Keep the read bounded even when the file grows after file_size().
    std::array<char, config_bytes_max + 1> buffer{};
    file.read(buffer.data(), buffer.size());
    if (file.bad() || (!file.eof() && file.fail()) ||
        static_cast<size_t>(file.gcount()) > config_bytes_max) {
        error = "配置读取失败或超过 16 KiB。";
        return false;
    }
    std::istringstream lines(std::string(buffer.data(), static_cast<size_t>(file.gcount())));
    std::set<std::string> seen;
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            error = "配置行必须为 key=value。";
            return false;
        }
        const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
        if (!seen.insert(key).second) {
            error = "配置项重复。";
            return false;
        }
        if (key == "provider") {
            if (value != "mock" && value != "openai") {
                error = "provider 只支持 mock/openai。";
                return false;
            }
            model.model = value == "mock" ? runtime::Model::mock : runtime::Model::remote;
        } else if (key == "base_url") {
            model.base_url = value;
        } else if (key == "model") {
            model.model_name = value;
        } else if (key == "key_environment") {
            model.key_environment = value;
        } else if (key == "quiet_timezone") {
            options.quiet.timezone = value;
        } else if (key == "quiet_hours") {
            options.quiet.enabled = value != "off";
            if (options.quiet.enabled && (value.size() != 11 || value[5] != '-' ||
                                          !minute(value.substr(0, 5), options.quiet.start_minute) ||
                                          !minute(value.substr(6), options.quiet.end_minute))) {
                error = "quiet_hours 使用 HH:MM-HH:MM 或 off。";
                return false;
            }
        } else {
            error = "未知配置项；不支持在文件中保存密钥。";
            return false;
        }
    }
    return true;
}
} // namespace
const char *usage() {
    return "用法：ayni [--config 文件] [--data-dir 目录] [--mock] [--help]\n"
           "默认配置：$XDG_CONFIG_HOME/ayni/config（或 ~/.config/ayni/config）\n"
           "默认数据：$XDG_DATA_HOME/ayni（或 ~/.local/share/ayni）\n"
           "无配置时使用本地 Mock；真实服务需 "
           "provider=openai、base_url、model、key_environment。\n";
}
bool read_options(int argc, char **argv, Options &out, std::string &error) {
    auto config_home = environment("XDG_CONFIG_HOME"), data_home = environment("XDG_DATA_HOME");
    const auto home = environment("HOME");
    if (config_home.empty()) {
        config_home = home.empty() ? "" : home + "/.config";
    }
    if (data_home.empty()) {
        data_home = home.empty() ? "" : home + "/.local/share";
    }
    if (!config_home.empty()) {
        out.config_file = config_home + "/ayni/config";
    }
    if (!data_home.empty()) {
        out.data_directory = data_home + "/ayni";
    }
    bool explicit_config = false, mock = false;
    for (int at = 1; at < argc; ++at) {
        const std::string arg = argv[at];
        if (arg == "--help") {
            out.help = true;
        } else if (arg == "--mock") {
            mock = true;
        } else if ((arg == "--config" || arg == "--data-dir") && at + 1 < argc) {
            if (arg == "--config") {
                out.config_file = argv[++at];
                explicit_config = true;
            } else {
                out.data_directory = argv[++at];
            }
        } else {
            error = "未知参数或缺少参数值。";
            return false;
        }
    }
    if (out.help) {
        return true;
    }
    if (out.config_file.empty() || out.data_directory.empty()) {
        error = "请设置 HOME/XDG 目录或显式指定配置与数据目录。";
        return false;
    }
    if (!load(out.config_file, explicit_config, out, error)) {
        return false;
    }
    if (mock) {
        out.model = {};
    }
    if (out.quiet.validate() != AYNI_OK) {
        error = "安静时段须为非空区间，并配置明确的 POSIX quiet_timezone。";
        return false;
    }
    if (out.model.model == runtime::Model::remote &&
        (out.model.base_url.empty() || out.model.model_name.empty() ||
         out.model.key_environment.empty())) {
        error = "真实服务配置缺少 base_url、model 或 key_environment。";
        return false;
    }
    return true;
}
} // namespace ayni::cli
