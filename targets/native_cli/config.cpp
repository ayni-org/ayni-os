#include "config.h"
#include <array>
#include <ayni/ayni.h>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
namespace ayni::cli {
namespace {
constexpr size_t config_bytes_max = 16384;
std::string environment(const char *name) {
    const auto *value = std::getenv(name);
    return value ? value : "";
}
// A whole POSIX TZ rule; one day after the epoch keeps west offsets valid.
bool posix_timezone(const std::string &zone) {
    constexpr int64_t zone_check_utc_s = 86400;
    int64_t local = 0;
    return !zone.empty() &&
           ayni_calendar_local_time({zone.data(), static_cast<uint32_t>(zone.size())},
                                    zone_check_utc_s, &local) == AYNI_OK;
}
// The POSIX rule a TZif v2+ file appends after its last data block, "\n<rule>\n"; empty when
// the file is missing, older or ends without one.
std::string tzif_rule(const std::string &path) {
    constexpr size_t tzif_bytes_max = 65536;
    constexpr std::string_view magic = "TZif";
    std::ifstream file(path, std::ios::binary);
    std::string bytes(tzif_bytes_max, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(file.gcount()));
    if (bytes.compare(0, magic.size(), magic) != 0 || bytes.back() != '\n') {
        return {};
    }
    const auto last = bytes.size() - 1; // The footer's closing newline.
    const auto start = bytes.rfind('\n', last - 1);
    const auto rule =
        start == std::string::npos ? std::string() : bytes.substr(start + 1, last - start - 1);
    return posix_timezone(rule) ? rule : std::string();
}
// The machine's zone as a POSIX rule (ADR-0073): TZ if it is a whole rule, else TZ without a
// leading ':' as a zone file (absolute, or under /usr/share/zoneinfo), or /etc/localtime when
// TZ is unset or empty. Main differences from glibc: a rule wins over a file of the same name, an
// empty TZ or ":" is not UTC, ":<rule>" is not parsed as a rule, and TZDIR is not read.
std::string system_timezone() {
    std::string zone = environment("TZ");
    if (posix_timezone(zone)) {
        return zone;
    }
    if (!zone.empty() && zone.front() == ':') {
        zone.erase(0, 1);
    }
    if (zone.empty()) {
        return tzif_rule("/etc/localtime");
    }
    return tzif_rule(zone.front() == '/' ? zone : "/usr/share/zoneinfo/" + zone);
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
        } else if (key == "timezone") {
            if (!posix_timezone(value)) {
                error = "timezone 须为 POSIX 时区规则，例如 CST-8。";
                return false;
            }
            model.timezone = value;
        } else if (key == "shell_working_directory") {
            model.shell_working_directory = value;
        } else if (key == "output_reserve_tokens") {
            error = "output_reserve_tokens 已改名为 max_output_tokens。";
            return false;
        } else if (key == "context_window_tokens" || key == "max_output_tokens" ||
                   key == "compact_target_tokens" || key == "tool_result_max_tokens" ||
                   key == "max_turns" || key == "request_timeout_ms" ||
                   key == "model_concurrency") {
            uint64_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !number) {
                error = key + " 须为正整数。";
                return false;
            }
            auto &h = model.harness;
            const auto small = [&](uint32_t &out) {
                if (number > UINT32_MAX) {
                    return false;
                }
                out = static_cast<uint32_t>(number);
                return true;
            };
            bool fits = true;
            if (key == "context_window_tokens") {
                h.context_window_tokens = number;
            } else if (key == "max_output_tokens") {
                h.max_output_tokens = number;
            } else if (key == "compact_target_tokens") {
                h.compact_target_tokens = number;
            } else if (key == "tool_result_max_tokens") {
                h.tool_result_max_tokens = number;
            } else if (key == "max_turns") {
                fits = small(h.max_turns);
            } else if (key == "request_timeout_ms") {
                fits = small(h.request_timeout_ms);
            } else {
                fits = small(h.model_concurrency);
            }
            if (!fits) {
                error = key + " 超出范围。";
                return false;
            }
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
    if (!runtime::valid(options.model.harness)) {
        error = "运行设置须满足：max_output_tokens < context_window_tokens，tool_result_max_tokens "
                "< 二者之差，compact_target_tokens < 再减工具结果之差；max_turns 1..64，"
                "request_timeout_ms 1..300000，model_concurrency 1..8。";
        return false;
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
    out.model.timezone = system_timezone();
    if (!load(out.config_file, explicit_config, out, error)) {
        return false;
    }
    if (mock) {
        runtime::ModelConfig local;
        local.timezone = out.model.timezone; // The local Mock is told the same time.
        local.shell_working_directory = out.model.shell_working_directory;
        out.model = local;
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
