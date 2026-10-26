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
bool agent(const std::string &word, uint64_t &out) {
    if (word != "a" && word != "b") {
        return false;
    }
    out = word == "a" ? runtime::demo_agent_a : runtime::demo_agent_b;
    return true;
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
    return "@a check                  运行 A 的预设规则（Mock）\n"
           "@b 文本                   与 B 对话（每条输入/回答最多 384 UTF-8 字节）\n"
           "/status /goals /tasks /calls /grants /conversation\n"
           "/grant a|b model|summary|present   创建长期授权（模型含外发/计费）\n"
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
           "/memory share <id> a|b    单条记忆/档案读授权，返回 Grant 可撤销\n"
           "/memory export <id> a|b   授权外发到当前模型目的地（需远程配置）\n"
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
