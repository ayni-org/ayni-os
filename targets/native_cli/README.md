# Native CLI

P2 的单进程 Linux 演示入口（ADR-0057）：A 执行预设 Mock 规则，B 使用 ToolLoop 对话。后台线程独占 Kernel，等待键盘输入时仍推进工作。它不是通用 C++ SDK 或统一助理。

先按 [开发指南](../../docs/development.md) 检查资源并设置构建/测试并行度，再执行：

```sh
cmake --preset native-debug
cmake --build --preset native-debug --target ayni
./build/native/targets/native_cli/ayni --mock
```

默认配置是 `$XDG_CONFIG_HOME/ayni/config`（回退 `~/.config/ayni/config`），数据是 `$XDG_DATA_HOME/ayni`（回退 `~/.local/share/ayni`）。`--config 文件`、`--data-dir 目录` 可覆盖。不存在配置时用 Mock；`--mock` 显式选 Mock，配置语法错误仍会拒绝启动。状态目录只允许当前用户访问，首次创建为 0700，对话文件为 0600；保留整个目录才能正常重开，不能只复制对话文件。

首次运行没有授权。可先输入 `@b 你好`，用 `/goals` 查看待批目标，再 `/approve <goal-id>` 为它批准缺少的权限。也可以提前授权：

```text
/grant a present
/wait
/grant b model
/wait
/grant b summary
/wait
/grant b present
/wait
@a check
/wait
@b 你好
/wait
/conversation
/quit
```

模型授权在真实服务配置下允许外发和计费；`present` 在本 Profile 中为本地 Mock 工具。`/wait` 等待当前命令、工作和文件保存，待审批/暂停的对象会保留。脚本中每个 mutation 后加 `/wait` 可避免前次文件保存期间返回 BUSY。EOF、Ctrl-C 与 `/quit` 都走正常关闭。

| 命令 | 用途 |
|---|---|
| `@a check` / `@b 文本` | 运行 A 的规则 / 与 B 对话 |
| `/status` | 生命周期、受限原因、持久进度与对话保存状态 |
| `/goals` `/tasks` `/calls` `/grants` | 查询仍保留的对象及诊断 ID |
| `/grant a\|b model\|summary\|present` | 创建长期授权 |
| `/revoke <grant-id>` | 撤销授权，受理回执包含阶段 |
| `/approve <goal-id>` | 批准目标所有缺少的权限 |
| `/reauthorize <goal-id>` | 用已有授权重新准入原目标 |
| `/limit <task-id> <次数>` | 提高模型调用次数上限 |
| `/resume <task-id>` | 恢复等待超时的原 Task |
| `/query <call-id>` | 向原 Provider 对账；不支持时明确返回错误，不重试调用 |
| `/cancel <goal-id>` | 取消目标 |
| `/conversation` / `/forget` | 显示 / 清空 B 当前对话与摘要 |
| `/wait` `/help` `/quit` | 等待、帮助、正常退出 |

真实模型的配置格式如下（最大 16 KiB，未知/重复键拒绝）：

```ini
provider=openai
base_url=https://api.deepseek.com
model=deepseek-chat
key_environment=AYNI_LLM_API_KEY
```

由本机环境提供 `AYNI_LLM_API_KEY`，启动时去掉 `--mock`。配置文件中只能写环境变量名，不能写密钥值；运行时不会自动切换服务或重试未知调用。模型服务仍使用 P1 的非流式 Chat Completions 接口。上述地址/模型是 P1 已单独验证的配置示例，不代表 P2 已完成真实服务多轮验收。

当前每条输入/回答上限 384 UTF-8 字节，保存最近 3 轮和至多 512 字节模型摘要；超限报错，不截断。摘要是未确认推断。正常退出重开继续使用原工作和已保存上下文；异常退出时 Kernel 日志与对话文件的跨库窗口尚未验证。`/forget` 只清当前外层值，Kernel 历史正文擦除及单轮删除留到 P3；终端通知也不承诺恰好一次。

退出码：0 正常，1 命令/运行失败，2 配置/启动参数错误。Mock 脚本验收：

```sh
ctest --preset native-debug -R '^NativeCli\.'
```
