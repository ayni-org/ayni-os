# Native CLI

单进程 Linux 统一助理入口（ADR-0057/0066/0072）：只有一个助理 Agent，所有自然语言都交给 ToolLoop；应用注册四类目标，模型也可经计划工具登记长期目标，但须所有者批准才运行。后台 owner 独占 Kernel，输入及有界输出队列独立推进。

先按 [开发指南](../../docs/development.md) 检查资源并设置构建/测试并行度，再执行：

```sh
cmake --preset native-debug
cmake --build --preset native-debug --target ayni
./build/native/targets/native_cli/ayni --mock
```

默认配置是 `$XDG_CONFIG_HOME/ayni/config`（回退 `~/.config/ayni/config`），数据是 `$XDG_DATA_HOME/ayni`（回退 `~/.local/share/ayni`）。`--config 文件`、`--data-dir 目录` 可覆盖。不存在配置时用 Mock；`--mock` 显式选 Mock，配置语法错误仍会拒绝启动。状态目录只允许当前用户访问，首次创建为 0700，对话文件为 0600；保留整个目录才能正常重开，不能只复制对话文件。

首次运行没有授权。可先输入 `你好`，用 `/goals` 查看待批目标，再 `/approve <goal-id>` 为它批准缺少的权限。也可以提前授权：

```text
/grant model
/wait
/grant summary
/wait
/grant schedule
/wait
你好
/wait
/conversation
/quit
```

模型授权在真实服务配置下允许外发和计费；`schedule` 是本地计划工具，只登记目标、不直接运行。`/wait` 等待当前命令、工作和文件保存，待审批/暂停的对象会保留。脚本中每个 mutation 后加 `/wait` 可避免前次文件保存期间返回 BUSY。EOF、Ctrl-C 与 `/quit` 都走正常关闭。

| 命令 | 用途 |
|---|---|
| 普通文本 | 与助理对话（ToolLoop）；`check/检查` 也是普通文本 |
| `停` `停止` `stop` / `/stop` | 不经模型立即取消进行中的回答、暂停自主工作；提醒保留 |
| `/status` / `/diagnostics` | 生命周期、受限原因、持久进度与对话保存状态 |
| `/goals` `/tasks` `/calls` `/grants` | 查询仍保留的对象及诊断 ID |
| `/grant model\|summary\|schedule\|notify\|health\|observe` | 创建长期授权 |
| `/revoke <grant-id>` | 撤销授权，受理回执包含阶段 |
| `/approve <goal-id>` | 批准目标所有缺少的权限 |
| `/reauthorize <goal-id>` | 用已有授权重新准入原目标 |
| `/limit <task-id> <次数>` | 提高模型调用次数上限 |
| `/resume <task-id>` | 恢复等待超时的原 Task |
| `/query <call-id>` | 向原 Provider 对账；不支持时明确返回错误，不重试调用 |
| `/cancel <goal-id>` | 取消目标 |
| `/conversation` / `/forget` | 显示 / 清空当前对话与摘要 |
| `/wait` `/help` `/quit` | 等待、帮助、正常退出 |

真实模型的配置格式如下（最大 16 KiB，未知/重复键拒绝）：

```ini
provider=openai
base_url=https://api.deepseek.com
model=deepseek-chat
key_environment=AYNI_LLM_API_KEY
```

时区用 `timezone=CST-8` 这样的 POSIX 规则配置（区域名如 `Asia/Shanghai` 会被拒绝）；不配置时依次取本机时区：`TZ` 本身是完整规则；`TZ`（去掉开头的 `:`）指向的时区文件，绝对路径或相对 `/usr/share/zoneinfo`；`TZ` 未设或为空时 `/etc/localtime` 末尾的规则；都取不到时模型被告知时间未知。与 glibc 不同，规则优先于同名文件，空 `TZ` 不当作 UTC，也不读 `TZDIR`。本机时钟未同步时仍写出时间并注明未同步（ADR-0073）。指令与当前时间作为 system 消息发送，对话作为 user 消息（ADR-0074）。

由本机环境提供 `AYNI_LLM_API_KEY`，启动时去掉 `--mock`。配置文件中只能写环境变量名，不能写密钥值；运行时不会自动切换服务或重试未知调用。模型服务仍使用 P1 的非流式 Chat Completions 接口。上述地址/模型是 P1 已单独验证的配置示例，不代表 P2 已完成真实服务多轮验收。

当前每条输入/回答上限 384 UTF-8 字节，保存最近 3 轮和至多 512 字节模型摘要；超限报错，不截断。摘要是未确认推断。正常退出重开继续使用原工作和已保存上下文；异常退出时 Kernel 日志与对话文件的跨库窗口尚未验证。`/forget [轮次]` 经 Kernel 屏障清理受管理正文；单轮删除保留其他原始输入、清除旧回答/摘要，下一轮按有效输入重建。`/diagnostics` 区分隐私清理进行中、完成及错误；`/wait` 等待文件 ACK 和双槽覆盖完成；终端通知也不承诺恰好一次。

退出码：0 正常，1 命令/运行失败，2 配置/启动参数错误。Mock 脚本验收：

```sh
ctest --preset native-debug -R '^NativeCli\.'
```


P3 记忆命令（ID 取命令返回值）：

```text
/memory save 喜欢简短的中文回答
/memory list
/memory show ID
/memory use ID 请按这个偏好回答
/memory edit ID 喜欢详细的中文回答
/memory confirm ID
/memory share ID
/memory export ID
/memory delete ID
/profile list
/profile save language 中文
```

`share` 给助理读这一条（档案同理）；远程模型还需 `export` 给实际配置的目的地。用 `/grants` 查许可 ID、`/revoke ID` 撤销。档案字段为 `name`、`language`、`interaction`，初始为空且需单独授权。助理通过 `/memory use` 显式选择记忆；模型主动记忆工具尚未接入。

显式保存即确认；模型推断和派生内容保留来源，只有所有者能确认。默认 64 条/每条 512 UTF-8 字节/8 项完整依赖，满容量拒绝。保留中的原始用户输入不随派生结果失效丢失；未完成输入显示为草稿。发布失效最多保留 4 个原始轮次，满时先 `/forget 轮次` 再继续；未编号草稿续写合计最多 384 字节，`/forget` 可明确清空。

P2 的旧对话文件没有来源证据，P3 会在任何启动写入前拒绝并保留该目录。双 Agent 版本（ADR-0072 之前）的数据目录同样在任何启动写入前拒绝，提示“不兼容的旧版本”。没有自动迁移；试用新版本可显式指定一个新数据目录。清理范围为本程序管理的逻辑文件/内存，不撤回远端已接收内容，也不保证介质取证擦除。

所有自然语言都交给同一个 ToolLoop 助理，不按关键词路由。四类目标命令：

```text
/remind once 2026-10-28T09:00 CST-8 喝水
/remind daily 2026-10-28T09:00 CST-8 喝水
/check 300
/tidy <memory-id> [memory-id...]
/observe
/pause-assistant
/resume-assistant
/notifications
/status
/diagnostics
```

`/remind`、`/check`、`/observe` 与模型计划工具共用同一文法：`remind once|daily|weekly YYYY-MM-DDTHH:MM <POSIX 时区> <内容>`、`check [秒，至少 300]`、`observe`。模型登记的目标始终待批：CLI 显示“助理登记了新目标 N，等待你批准”（重新打开时对仍待批的再显示一次），`/approve N` 后才运行，`/cancel N` 放弃。待批的模型目标最多 4 个，满了助理会请你先处理。每次请求都会告诉模型当前本地日期、星期和时区，“明天”“下周三”由模型自行换算，登记的目标仍要你看过后批准。`/tidy` 只由所有者发起。

四类目标都保留授权审批；先用 `/goals` 查看待批 ID，再明确 `/approve`，或事先 `/grant notify`、`/grant health`、`/grant summary`、`/grant observe`。整理最多四条本人私有记忆或已获 ORGANIZE 授权的公共档案；远程模型另需每条来源的 `/memory export <id> organize`，不会自动外发。持续观察仅是 Mock 标量，不是图像识别。

“停”“停下”“停止”“stop”（可带句末标点）以及 `/stop` 走确定性停止：取消进行中的回答（如有），暂停自主检查/观察/整理，已接受的提醒继续；不调用模型，也不受对话保存未完成的限制。停止时助理尚未决定的计划请求不会登记目标。`/resume-assistant` 恢复后台工作。

配置可加入 `quiet_hours=22:00-07:00` 与 `quiet_timezone=CST-8`；默认关闭，也可显式 `quiet_hours=off`。普通通知待可信校时及窗口开放；已开始写出的通知允许结束。CLI 输出线程完整写出并 flush 成功才是呈现证据；失败/未知不自动重播。明确取消待处理提醒后再创建，是新的用户意图。

`/wait` 等当前可推进工作及持久/文件提交，不等待未来提醒、下一周期或正在等待新订阅样本的持续观察。`/status` 显示并行活动与“自主暂停，提醒继续”；内部 ID 和资源/存储信息使用诊断命令。提醒使用独立 `notify` 能力。

普通输出入有界队列，owner 不持 stdout 锁；提醒等待实际刷新回执。Console 排空最多等待两秒，失败以退出码1直接结束进程并跳过标准流退出刷新；这是失败退出，不作为正常关闭或通知成功证据。
