#include "commands.h"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
namespace ayni::cli {
namespace {
bool number(const std::string &word, uint64_t &out) {
    const auto result = std::from_chars(word.data(), word.data() + word.size(), out);
    return result.ec == std::errc{} && result.ptr == word.data() + word.size() && out != 0;
}
bool agent(const std::string &word, uint64_t &out) {
    if (word != "a" && word != "b") {
        return false;
    }
    out = word == "a" ? runtime::demo_agent_a : runtime::demo_agent_b;
    return true;
}
bool reminder(std::istringstream &input, runtime::Command &c) {
    std::string kind, date, zone;
    if (!(input >> kind >> date >> zone) || zone.size() > AYNI_CALENDAR_TZ_MAX_BYTES) {
        return false;
    }
    std::tm civil{};
    std::istringstream fields(date);
    fields >> std::get_time(&civil, "%Y-%m-%dT%H:%M");
    if (fields.fail() || fields.peek() != std::char_traits<char>::eof()) {
        return false;
    }
    const auto expected = civil;
    const auto seconds = timegm(&civil);
    if (seconds < 0 || seconds > AYNI_CALENDAR_SECONDS_MAX || civil.tm_year != expected.tm_year ||
        civil.tm_mon != expected.tm_mon || civil.tm_mday != expected.tm_mday ||
        civil.tm_hour != expected.tm_hour || civil.tm_min != expected.tm_min) {
        return false;
    }
    auto &r = c.calendar;
    r.struct_size = sizeof(r);
    r.kind = kind == "once"     ? AYNI_CALENDAR_ONCE
             : kind == "daily"  ? AYNI_CALENDAR_DAILY
             : kind == "weekly" ? AYNI_CALENDAR_WEEKLY
                                : 0;
    if (!r.kind) {
        return false;
    }
    r.rule_version = 1;
    r.first_local_s = seconds;
    r.timezone_size = static_cast<uint32_t>(zone.size());
    std::copy(zone.begin(), zone.end(), r.timezone);
    std::getline(input >> std::ws, c.text);
    c.operation = runtime::Operation::remind;
    c.agent = runtime::demo_agent_a;
    return !c.text.empty();
}
bool autonomous(const std::string &op, std::istringstream &input, runtime::Command &c) {
    std::string word;
    c.operation = op == "/check"  ? runtime::Operation::device_check
                  : op == "/tidy" ? runtime::Operation::memory_organize
                                  : runtime::Operation::scene_observe;
    c.agent = op == "/check" ? runtime::demo_agent_a : runtime::demo_agent_b;
    if (input >> word) {
        if (!agent(word, c.agent)) {
            return false;
        }
    } else {
        return op != "/tidy";
    }
    if (op == "/tidy") {
        while (input >> word) {
            uint64_t id = 0;
            if (!number(word, id) ||
                c.sources.size() == applications::assistant::memory_sources_max) {
                return false;
            }
            c.sources.push_back(id);
        }
        return !c.sources.empty();
    }
    if (op == "/check" && (input >> word)) {
        uint64_t seconds = 0;
        if (!number(word, seconds) || seconds > UINT64_MAX / 1000000) {
            return false;
        }
        c.period_us = seconds * 1000000;
    }
    return !(input >> word);
}
bool memory(std::istringstream &input, runtime::Command &c) {
    std::string op, value, extra;
    if (!(input >> op)) {
        return false;
    }
    if (op == "list" || op == "save") {
        c.operation =
            op == "list" ? runtime::Operation::memory_list : runtime::Operation::memory_save;
        if (!(input >> value) || !agent(value, c.agent)) {
            return false;
        }
        std::getline(input >> std::ws, c.text);
        return op == "list" || !c.text.empty();
    }
    if (!(input >> value) || !number(value, c.id)) {
        return false;
    }
    if (op == "show") {
        c.operation = runtime::Operation::memory_show;
    } else if (op == "confirm") {
        c.operation = runtime::Operation::memory_confirm;
    } else if (op == "delete") {
        c.operation = runtime::Operation::memory_delete;
    } else if (op == "share" || op == "export") {
        c.operation =
            op == "share" ? runtime::Operation::memory_share : runtime::Operation::memory_export;
        if (!(input >> value) || !agent(value, c.agent)) {
            return false;
        }
        if (input >> value) {
            if (value != "organize") {
                return false;
            }
            c.purpose = AYNI_MEMORY_ORGANIZE;
        }
    } else if (op == "edit" || op == "use") {
        c.operation = op == "edit" ? runtime::Operation::memory_edit : runtime::Operation::say;
        std::getline(input >> std::ws, c.text);
        return !c.text.empty();
    } else {
        return false;
    }
    return !(input >> extra);
}
bool profile(std::istringstream &input, runtime::Command &c) {
    std::string op, key;
    if (!(input >> op)) {
        return false;
    }
    if (op == "list") {
        c.operation = runtime::Operation::profile_list;
        std::getline(input >> std::ws, c.text);
        return true;
    }
    if (op != "save" || !(input >> key)) {
        return false;
    }
    c.profile_field = key == "name"          ? AYNI_PROFILE_NAME
                      : key == "language"    ? AYNI_PROFILE_LANGUAGE
                      : key == "interaction" ? AYNI_PROFILE_INTERACTION
                                             : AYNI_PROFILE_NONE;
    if (c.profile_field == AYNI_PROFILE_NONE) {
        return false;
    }
    c.operation = runtime::Operation::profile_save;
    std::getline(input >> std::ws, c.text);
    return !c.text.empty();
}
} // namespace
const char *help() {
    return "普通文本自动路由；check/检查→A，其余→B；@a/@b 点名优先。\n"
           "/remind once|daily|weekly YYYY-MM-DDTHH:MM POSIX-TZ 提醒内容\n"
           "/check [a|b [周期秒]]      周期设备检查（Mock，至少 300 秒）\n"
           "/tidy a|b <记忆-id> [id...] 低优先级整理最多 4 条记忆，只生成未确认候选\n"
           "/observe [a|b]           持续观察 Mock 变化，模型次数沿用 Task 上限\n"
           "/pause-assistant /resume-assistant  暂停/恢复自主工作，提醒保留\n"
           "/notifications /status    通知呈现状态 / 助理状态\n"
           "/diagnostics              Kernel、存储与对话诊断\n"
           "@a check                  运行 A 的预设规则（Mock）\n"
           "@b 文本                   与 B 对话（每条输入/回答最多 384 UTF-8 字节）\n"
           "/status /goals /tasks /calls /grants /conversation\n"
           "/grant a|b model|summary|present|notify|health|observe  创建长期授权\n"
           "/revoke <grant-id>         撤销授权\n"
           "/approve <goal-id>         批准该目标所有缺少的权限\n"
           "/reauthorize <goal-id>     用已有授权重新准入原目标\n"
           "/limit <task-id> <次数>    提高模型次数上限\n"
           "/resume <task-id>          恢复等待超时的原 Task\n"
           "/query <call-id>           向原 Provider 查询未知结果（需其支持）\n"
           "/cancel <goal-id>          取消目标\n"
           "/forget [轮次]            删除 B 全部/指定轮次，清除衍生摘要和回答\n"
           "/memory list a|b [关键词] /memory show <id>\n"
           "/memory save a|b 正文 /memory edit <id> 正文\n"
           "/memory confirm <id> /memory delete <id>\n"
           "/memory share <id> a|b [organize]  单条记忆/档案读授权，可撤销\n"
           "/memory export <id> a|b [organize] 授权外发到当前模型目的地\n"
           "/memory use <id> 提问     B 在本轮显式读取这条记忆\n"
           "/profile list /profile save name|language|interaction 正文\n"
           "/wait                     等待当前工作与文件提交；待批/暂停工作保留\n"
           "/help /quit               帮助 / 正常退出\n";
}
bool parse(const std::string &line, Request &out) {
    out = {};
    std::istringstream input(line);
    std::string op, arg, extra;
    if (!(input >> op)) {
        return false;
    }
    auto &c = out.command;
    if (op == "@a" || op == "@b") {
        c.operation = runtime::Operation::say;
        c.agent = op == "@a" ? runtime::demo_agent_a : runtime::demo_agent_b;
        std::getline(input >> std::ws, c.text);
        return !c.text.empty();
    }
    if (op[0] != '/' && op[0] != '@') {
        c.operation = runtime::Operation::say;
        c.agent = 0;
        c.text = line;
        return true;
    }
    if (op == "/remind") {
        return reminder(input, c);
    }
    if (op == "/check" || op == "/tidy" || op == "/observe") {
        return autonomous(op, input, c);
    }
    if (op == "/memory") {
        return memory(input, c);
    }
    if (op == "/profile") {
        return profile(input, c);
    }
    if (op == "/help") {
        out.action = Action::help;
    } else if (op == "/quit") {
        out.action = Action::quit;
    } else if (op == "/wait") {
        out.action = Action::wait;
    } else if (op == "/status") {
        c.operation = runtime::Operation::status;
    } else if (op == "/diagnostics") {
        c.operation = runtime::Operation::diagnostics;
    } else if (op == "/notifications") {
        c.operation = runtime::Operation::notifications;
    } else if (op == "/pause-assistant") {
        c.operation = runtime::Operation::pause_assistant;
    } else if (op == "/resume-assistant") {
        c.operation = runtime::Operation::resume_assistant;
    } else if (op == "/goals") {
        c.operation = runtime::Operation::goals;
    } else if (op == "/tasks") {
        c.operation = runtime::Operation::tasks;
    } else if (op == "/calls") {
        c.operation = runtime::Operation::calls;
    } else if (op == "/grants") {
        c.operation = runtime::Operation::grants;
    } else if (op == "/conversation") {
        c.operation = runtime::Operation::conversation;
    } else if (op == "/forget") {
        c.operation = runtime::Operation::erase;
        if (input >> arg) {
            if (!number(arg, c.id)) {
                return false;
            }
        }
    } else if (op == "/grant") {
        c.operation = runtime::Operation::grant;
        if (!(input >> arg >> c.text) || !agent(arg, c.agent)) {
            return false;
        }
    } else {
        if (op == "/approve") {
            c.operation = runtime::Operation::approve;
        } else if (op == "/reauthorize") {
            c.operation = runtime::Operation::reauthorize;
        } else if (op == "/revoke") {
            c.operation = runtime::Operation::revoke;
        } else if (op == "/limit") {
            c.operation = runtime::Operation::raise_limit;
        } else if (op == "/resume") {
            c.operation = runtime::Operation::resume_wait;
        } else if (op == "/query") {
            c.operation = runtime::Operation::query_result;
        } else if (op == "/cancel") {
            c.operation = runtime::Operation::cancel;
        } else {
            return false;
        }
        if (!(input >> arg) || !number(arg, c.id)) {
            return false;
        }
        if (op == "/limit") {
            uint64_t limit = 0;
            if (!(input >> arg) || !number(arg, limit) ||
                limit > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            c.limit = static_cast<uint32_t>(limit);
        }
    }
    return !(input >> extra);
}
} // namespace ayni::cli
