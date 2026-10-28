# ADR-0057：双 Agent Native runtime、对话文件与 CLI

- **状态**：已接受，P2 实现与本地正常验证完成；双 Agent 装配与 `@a/@b` 点名已由 [ADR-0072](0072-single-assistant-schedule-tool-and-stop.md) 取代
- **日期**：2026-10-26
- **关联**：MA-020 前半；ADR-0010、0042 决定 13、0053 F16、0056；C-LIFE-030、C-NATIVE-001～006

## 背景

P1 已有规则、ToolLoop、Native 模型 Provider 和有界对话值，但可执行目标只做生命周期演示。P2 需要实际双 Agent 使用、管理命令和正常重开；公开 API 原来只有按 ID 查询，完成 Task 的对话结果仍由外层负责保存。

## 决策

1. **交付边界。** 两个 PR：第一项交付可独立测试的 Native/Sim 演示装配、管理操作与对话存储；第二项交付 CLI 输入/展示、配置和脚本会话。共享 Profile 在 `runtime/common` 的独立 Native target，ESP-IDF 仍只使用 header-only 默认配置。`NativeNode` 增加显式配置重载；`SimNode` 持有独立 FakeClock/存储，正常 release/reassemble 保留存储。Core 不引用具体 Provider 或业务 GoalType。
2. **最小公开查询，ABI 0.39。** `ayni_list_objects` 复制当前保留 Goal/Task/Call/Grant 的全部 ID；无排序保证，caller 提供有界缓冲，容量不足只报告所需数量。调用限拥有者线程、回调外，与其他查询同生命周期；枚举与逐项查询放在两次 mutation 之间，不承诺跨 step 快照或无限历史。受信管理接口不是远程授权边界。新增符号、既有结构布局及 Kernel arena 均不变；schema 2 不变，0.x 消费者按既定规则重编译。
3. **运行时与线程。** `Assistant` 是首个 Native 演示应用运行时，拥有装配生命周期、公开意图适配和对话提交协调；只返回 typed Reply/Notice，中文、命令和终端格式留在 `targets/native_cli`。它由 CLI 后台线程单独拥有，输入线程通过一个有界请求槽交换复制值；文件、网络各自有 I/O worker，`ayni_step` 不做同步 I/O。替换 Provider 改 Profile/配置；增设 UI 可复用 typed 操作，不需要解析 CLI 文本。完整通用 SDK 仍随 P5，不为本次演示增加插件或应用框架。
4. **所有者与授权。** 本机有效用户及其私有状态目录构成本地信任入口；启动注册固定所有者和 A/B，等绑定持久后开管理 Session。不自动创建 Grant；`grant`、`approve`、`revoke`、提高次数、恢复等待及对账全部提交原有意图。A 只做预设规则 `check`，B 使用 ToolLoop；每个 Agent 至多一个未完成前台工作，A/B 可并行推进。两者待批工作正常重开后继续使用原 Goal。命令 ID 是 `boot_epoch * 2^32 + 本 boot 的正序号`，耗尽显式拒绝；这是 CLI 独占目录下的管理键，不读取或构造 Kernel 对象 ID。既有调用未知不自动重试或换 Provider。
5. **对话文件。** 复用 `Conversation::encode/decode/finish/erase`；版本 1 文件 `conversation.bin` 为 `AYNF`、u32 版本、u64 正文字节数、u64 pending Goal ID（小端），后接既有 AYNC 编码。目录 0700、文件 0600；持有 Native Host 目录锁之后才读写。文件 worker 先写临时文件、fdatasync、rename，再 fsync 目录；保存成功才发布新上下文和答案。一次只排一个候选；保存期间拒绝新 mutation，避免新 Goal 回收尚未导出的终态 Task。未完成 Goal 的对应关系同文件保留；缺文件但已有 B 工作、owner 不匹配或未知格式均显式拒绝，不从历史猜测重建。文件保存与 Kernel journal 不是跨库事务；异常退出窗口、文件损坏与恢复专项未验收，不作断电一致性承诺。
6. **摘要与 Mock。** Native Provider 的启动配置显式选择 `TOOL_CALLS/openai.chat` 或 `TEXT/openai.text`，摘要用后者，不向摘要调用提供工具。Mock 增加复制的固定回复配置，以已有 A/B 的调度和完成端口验证多轮；这不是语言理解或摘要质量。真实 Provider 的工具链与摘要使用回环协议夹具，实 API 多轮由用户本机独立试用。保留 P1 的 384 字节输入/回答、最近 3 轮、512 字节摘要；超限报错，不截断。
7. **关闭。** 先收敛文件候选并驱动 Kernel 到 STOPPED；Provider `quiesce` 在 terminal post 已返回后 join 工作线程，保留注册 ctx；再销毁 Kernel/Host，最后释放 Profile。不能从“终态已消费”推断 post 已返回。未完成 turn 保留 pending Goal，正常重开按公开状态继续；不声称正常退出能撤回已外发请求。失败保留对象并报告，不能释放仍借用的内存。
8. **CLI 产品选择（用户 2026-10-26 选定推荐项）。** `@a/@b` 点名，英文短命令、中文提示；配置默认 `$XDG_CONFIG_HOME/ayni/config`（回退 `~/.config/ayni/config`），数据默认 `$XDG_DATA_HOME/ayni`（回退 `~/.local/share/ayni`），支持显式路径覆盖。文件只保存 provider/base_url/model/key_environment，密钥值只由 Provider 从环境读取；无配置时明确使用 Mock，配置错误不自动退回 Mock。整段清除仅清当前外层值，Kernel 历史正文擦除和单轮删除仍属 P3。（自 ADR-0072 起 Native 只有一个助理，`@a/@b` 取消；配置、目录与密钥规则不变。）

## 后果

- P2 增加实际使用入口及可复用装配，未交付长期私有记忆、日历、统一助理路由或完整 SDK。
- 对话正文不进入诊断日志；CLI 会按用户请求展示当前对话。模型、输入或配置不作为终端控制序列执行。
- 当前展示通知不是 P5 的持久呈现协议；正常重开会保留任务与对话，但不宣称终端输出恰好一次。
- 两个 PR 的边界可分别测试；生产改动较大，按查询、Profile、文件/运行时、CLI 四个不变量/边界组织审查。
- `/wait` 用公开 receipt 跟踪本 boot 接受的命令直到 DURABLE，包含 commit_seq 为零的 PENDING 撤销；同时等待工作与文件提交，待审批/暂停对象可以保留。客户端只暂存有界命令身份，不复制 Core 的授权或命令事实。
- Native 演示 Profile 使用 16 Goal/Task、64 Step/Call、4096 字节参数/检查点与 8192 字节调用输入/结果，保留 P1 的对话限制；它不是 ESP32 的默认内存配置。ABI 0.39 的四组既有内存配置与 0.38 相同，演示 Profile 的量测见 PR。

## 备选方案

- CLI 维护另一套对象目录：会漏掉重开恢复与 Kernel 回收，改为公共有界查询。
- 把对话正文作为 Core 新持久实体：违反 P1/P2 已定分工，延后到 P3 的隐私机制。
- 在输入线程直接查询 Kernel，或同步保存文件后才继续 step：破坏单拥有者/异步边界，未采用。
- 独立守护进程和网络管理 API：用户已选择单进程，留给后续阶段。

## 验证

Native/Sim 装配、公开查询、正常重开、多轮文件与回环 HTTP 工具/摘要链通过本地正常功能验证；独立只读审查发现均已修复并复查。CLI 五个脚本场景通过，包含双 Agent、摘要/重开、目录覆盖、配置与命令拒绝、Ctrl-C 正常退出。完整命令与 CI 结果见 runtime [#77](https://github.com/RobotSe7en/ayni/pull/77) 和 CLI [#78](https://github.com/RobotSe7en/ayni/pull/78)。新增故障/恢复专项、完整 E1–E4、ESP32、硬件及真实服务多轮不在本次验证范围。

2026-10-27 合并后覆盖率补测：现有完整套件及模拟器通过，Core 行覆盖达到通用目标；扩展到授权、资源、工作、Kernel、持久化与控制门的安全相关分支覆盖尚低于 90%，保留差距，不自动恢复已暂停的故障/恢复补测。范围、方法、结果与仓库外原始证据边界见 [#77](https://github.com/RobotSe7en/ayni/pull/77) 的合并后覆盖率记录；历史 E+F 的较窄范围不直接用于比较。
