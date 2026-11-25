#include "commands.h"
#include <charconv>
#include <limits>
#include <sstream>
namespace ayni::cli {
namespace {
bool number(const std::string &word, uint64_t &out) {
    const auto result = std::from_chars(word.data(), word.data() + word.size(), out);
    return result.ec == std::errc{} && result.ptr == word.data() + word.size() && out != 0;
}
// /tidy names explicit memory sources; it is owner-only and outside the schedule grammar.
bool organize(std::istringstream &input, runtime::Command &c) {
    c.operation = runtime::Operation::memory_organize;
    std::string word;
    while (input >> word) {
        uint64_t id = 0;
        if (!number(word, id) || c.sources.size() == applications::assistant::memory_sources_max) {
            return false;
        }
        c.sources.push_back(id);
    }
    return !c.sources.empty();
}
bool memory(std::istringstream &input, runtime::Command &c) {
    std::string op, value, extra;
    if (!(input >> op)) {
        return false;
    }
    if (op == "list" || op == "save") {
        c.operation =
            op == "list" ? runtime::Operation::memory_list : runtime::Operation::memory_save;
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
    return "普通文本交给助理（ToolLoop），长度只受 token 配置限制。\n"
           "停 / 停止 / stop 或 /stop  立即取消当前回答并暂停后台工作，不经过模型；提醒保留\n"
           "/remind once|daily|weekly YYYY-MM-DDTHH:MM POSIX-TZ 提醒内容\n"
           "/check [周期秒]            周期设备检查（Mock，至少 300 秒）\n"
           "/observe                  持续观察 Mock 变化，模型次数沿用 Task 上限\n"
           "/tidy <记忆-id> [id...]   低优先级整理最多 4 条记忆，只生成未确认候选\n"
           "助理也可以用同样的格式登记提醒/检查/观察，登记后须 /approve 才运行\n"
           "/pause-assistant /resume-assistant  暂停/恢复自主工作，提醒保留\n"
           "/notifications /status    通知呈现状态 / 助理状态\n"
           "/diagnostics              Kernel、存储与对话诊断\n"
           "/status /goals /tasks /calls /grants /conversation\n"
           "/grant model|summary|schedule|notify|health|observe|shell  创建长期授权\n"
           "/revoke <grant-id>         撤销授权\n"
           "/approve <goal-id>         批准该目标所有缺少的权限\n"
           "/reauthorize <goal-id>     用已有授权重新准入原目标\n"
           "/limit <task-id> <次数>    提高模型次数上限\n"
           "/resume <task-id>          恢复等待超时的原 Task\n"
           "/query <call-id>           向原 Provider 查询未知结果（需其支持）\n"
           "/unresolved               列出未决操作\n"
           "/ack <call-id>            人工结案（不表示副作用已查清）\n"
           "/cancel <goal-id>          取消目标\n"
           "/forget [轮次]            删除全部/指定轮次，清除衍生摘要和回答\n"
           "/memory list [关键词] /memory show <id>\n"
           "/memory save 正文 /memory edit <id> 正文\n"
           "/memory confirm <id> /memory delete <id>\n"
           "/memory share <id> [organize]   单条记忆/档案读授权，可撤销\n"
           "/memory export <id> [organize]  授权外发到当前模型目的地\n"
           "/memory use <id> 提问     本轮显式读取这条记忆\n"
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
    if (op[0] != '/') {
        c.operation = runtime::Operation::say;
        c.text = line;
        return true;
    }
    // The runtime parses the shared schedule grammar; the command is that line without '/'.
    if (op == "/remind" || op == "/check" || op == "/observe") {
        c.operation = runtime::Operation::schedule;
        c.text = line.substr(line.find('/') + 1);
        return true;
    }
    if (op == "/tidy") {
        return organize(input, c);
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
    } else if (op == "/stop") {
        c.operation = runtime::Operation::stop;
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
    } else if (op == "/unresolved") {
        c.operation = runtime::Operation::unresolved_ops;
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
        if (!(input >> c.text)) {
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
        } else if (op == "/ack") {
            c.operation = runtime::Operation::acknowledge_op;
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
