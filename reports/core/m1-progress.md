# M1 执行计划与进度

- **关联**：计划 §9、§9.1、§14；`docs/workflow.md`；`reports/core/m0-review.md` §5（每个工作包记下开始与结束日期）
- **更新**：每个 PR 合并时更新本文的 PR 表、工作包日期与“交接状态”。日期为合并提交的日期。
- **交接**：当前交接流程见 `docs/workflow.md` 第 7 节；本页记录已合并基线及 `MA-006` 的接手实施状态。接手时先读下文“交接状态”，本轮按用户 2026-10-23 指示完成剩余整理及 E/F；以当前交接首条为准。
- **范围**：M1 的目标是在电脑上完成 Core，并通过 E1–E4（计划 §9 的 M1 行）。工作包的内容、依赖与完成定义以计划 §9.1 为准，本文只记顺序、拆分、已定决定与进度。

## 执行顺序

用户 2026-10-19 认可按依赖分六步，每个工作包至少一个 PR：

| 步骤 | 工作包 | 内容 | 状态 |
|---|---|---|---|
| 1 | `MA-003`、`MA-005` | 事件入口、完成槽、Kernel 生命周期与启动恢复、模拟 Host | 完成 |
| 2 | `MA-004`、`MA-010` | 工作模型（主体、会话、Agent、Goal、Task、触发、事件、策略、步骤与检查点）；持久化与恢复 | `MA-004`、`MA-010` 完成 |
| 3 | `MA-006`、`MA-018`、`MA-008` | 权限与审批、资源与执行许可、Provider 调用；按依赖做 `006`，再交错做 `018A → 008A → 018B`，008C/008B 按 2026-10-26 的新顺序分属 P1/P4（ADR-0042 决定 11） | `MA-006` A–F 机制及 R1～R4 整理、018A、008A、018B 的正常功能已交付；008C（typed 工具调用 #73、Native Provider #75）已交付，008B 正常功能已随 P4 #83/#84 交付 |
| 4 | `MA-009`、`MA-011` | 双策略与顺序 Skill、私有记忆与删除 | MA-009 正常功能已交付 #76；MA-011A/B 正常功能已随 #81/#82 交付 |
| 5 | `MA-019`、`MA-020` | 助理协调器、SDK 与接入示例 | P5 正常功能完成（#86～#89）；P6 正常联合证据见 #93 |
| 6 | `MA-021`、`MA-022`、`MA-023`、`MA-033` | E1、E2、E3、E4 实验，然后写 Core Alpha 报告（`reports/core/`） | 正常子场景见 #90；E2/E1 Native 完整实验见 #98/#99，E3 交付见 #100；E4 Native/Sim 证据见 #101（ADR-0079），[Alpha Native 报告](m1-core-alpha.md) 汇总本轮证据与保留事项 |
| 之后 | `MA-038` | 多节点确定性仿真；留在 M1（用户选择按原计划），在第 6 步之后做，不阻断 M2，M5 前必须完成；编写解码器前须定 IDL 与线格式（ADR-0009） | typed 正常子集见 #92；完整包未完成 |

2026-10-26 起剩余工作按 P0～P6 推进（用户选定先做文本纵切，ADR-0042 决定 11）：上表各步骤的工作包归属不变，先后以 [M1 剩余实施顺序](m1-remaining-implementation.md) 为准。

`MA-035`（可选的具身 Profile Mock 与 R1.a）不做：它依赖可选的 `MA-034`，后者未做。

## 已合并的 PR

| PR | 日期 | 工作包与部分 | 内容 | ADR |
|---|---|---|---|---|
| #8 | 2026-10-19 | `MA-003` | 有界事件入口与完成槽，ABI 0.3 | ADR-0015 |
| #9 | 2026-10-19 | `MA-005` | 启动、恢复、`ayni_step` 与关闭 | ADR-0016 |
| #10 | 2026-10-20 | `MA-004` 第一部分 | 主体、会话、Agent 实例、GoalType 与 Goal 意图 | ADR-0017 |
| #11 | 2026-10-20 | `MA-004` 第二部分 B1 | Task 派生，暂停、恢复与停止的传播 | ADR-0018 |
| #12 | 2026-10-20 | `MA-004` 第二部分 B2 | 延时与周期触发 | ADR-0019 |
| #13 | 2026-10-20 | `MA-004` 第二部分 B2 | Goal 失效与 occurrence 的有界等待 | ADR-0020 |
| #14 | 2026-10-20 | `MA-004` 第二部分 B3 | 本地输入事件的投递与触发 | ADR-0021 |
| #15 | 2026-10-20 | `MA-004` 第三部分 C1 | 策略注册与决定 | ADR-0022 |
| #16 | 2026-10-20 | `MA-004` 第三部分 | Goal 随最后一个 Task 的取消或最后一个 occurrence 的跳过结束 | ADR-0023 |
| #17 | 2026-10-20 | `MA-004` 第三部分 C2 | 步骤与策略私有检查点 | ADR-0024 |
| #18 | 2026-10-21 | `MA-010` A | Goal 表满时终态 Goal 让位 | ADR-0025 |
| #19 | 2026-10-21 | 流程文档 | 开发流程（`docs/workflow.md`）与本文 | — |
| #20 | 2026-10-21 | `MA-010` B+C+D | 每个 transition 一个日志批次、整批暂存的重放与校验、重启后重新计时、目录缺失时保留并停下、Native 文件存储、命令幂等，ABI 0.14 | ADR-0026 |
| #21 | 2026-10-21 | `MA-010` E+F | 同身份重试、写入栅栏与重载、物理游标及准确容量错误、分页双槽快照、日志轮转与写频、schema 0→1 迁移及模拟旧 reader 拒绝，ABI 0.15 | ADR-0027 |
| #23 | 2026-10-21 | `MA-010` E+F 整理 | 只改结构、注释与测试，运行行为不变：存储预算与维护状态收进 `kernel/storage/`，命名常数，合并重复的预留与编解码，补注释；内核内存需求减 8 字节（重排字段少了对齐填充）；按变异测试为漏测的路径补 18 个测试 | — |
| #24 | 2026-10-22 | 流程文档 | 每个部分一个 PR；内核新增字段写明归属；植入缺陷由另一个会话出题；独立审查加查删掉的注释、内核新增字段、重复公式与裸数字 | — |
| #25 | 2026-10-22 | `MA-006` A | 可信 Host 会话入口、持久 owner/revision 绑定、失配只读受限与 legacy 恢复工作授权禁行，ABI 0.16；合并提交 `f297941` | ADR-0028 |
| #26 | 2026-10-22 | `MA-006` B | Capability 有界注册与有限 typed Scope 规范化/覆盖；明确字段类型、单位和完整请求规则，ABI 0.17；合并提交 `00a4126` | ADR-0029 |
| #27 | 2026-10-22 | `MA-006` C | Grant、受限委派、typed 查询、确切命令身份与持久恢复，ABI 0.18；代码及最终文档 CI 各八项全过，合并提交 `b1330325012c5087424843f67ae2278cd4ca686f` | ADR-0030 |
| #28 | 2026-10-22 | `MA-006` D | 即时 Grant 撤销、独立 Gate/pending/pin、阶段回执、有界持久清理与兼容恢复，ABI 0.19 | ADR-0031 |
| #29 | 2026-10-22 | 复查记录 | `MA-006` A–D 的复查结果与整理任务、要写进 `AGENTS.md` 的可读性约束与机器检查（`reports/core/ma006-a-d-review.md`） | — |
| #30 | 2026-10-23 | `MA-006` R1 | 可读性约束、机械检查与遗留基线、整改及 E/F 实施计划；已合并 | ADR-0032 |
| [#31](https://github.com/RobotSe7en/ayni/pull/31) | 2026-10-23 | `MA-006` R2a1 | Kernel ID、Grant revision 与记录布局归属整理；本地验证、独立审查与远端 CI 通过，已合并 | ADR-0033 |
| [#35](https://github.com/RobotSe7en/ayni/pull/35) | 2026-10-23 | `MA-006` R2a2 | 字段值 codec、Scope 与注册准入复用；本地五配置、固件编译、独立变异与审查通过 | ADR-0034 |
| [#42](https://github.com/RobotSe7en/ayni/pull/42)～[#45](https://github.com/RobotSe7en/ayni/pull/45)、[#47](https://github.com/RobotSe7en/ayni/pull/47) | 2026-10-23～24 | `MA-006` E | 需求事实与持久闭环、公开待批/查询、专属 Grant、原子批准与最低消费门控；各 PR 独立验证/审查 | ADR-0037 |
| [#49](https://github.com/RobotSe7en/ayni/pull/49) | 2026-10-24 | `MA-006` F1 | 内部授权事实、Task 有界引用表与 codec；本地验证、独立变异/审查及适用 CI 通过 | ADR-0038 |
| [#51](https://github.com/RobotSe7en/ayni/pull/51) | 2026-10-24 | `MA-006` F2 | Goal 绑定、独立 Task 准入、schema 2 持久配对及消费/查询，ABI 0.24 | ADR-0038 |
| [#52](https://github.com/RobotSe7en/ayni/pull/52) | 2026-10-24 | `MA-006` F3 | 有界持久清理、独立授权暂停、专属 Grant 终态/回收与输入/触发边界，ABI 0.25 | ADR-0038 |
| [#53](https://github.com/RobotSe7en/ayni/pull/53)、[#54](https://github.com/RobotSe7en/ayni/pull/54) | 2026-10-24 | `MA-006` F4 | 内部授权结果/持久证明及完整原子重新授权，ABI 0.26 | ADR-0038 |
| [#55](https://github.com/RobotSe7en/ayni/pull/55) | 2026-10-24 | `MA-006` F5 | 完整事实正常重启、兼容 hold 与资源边界，ABI 0.27 | ADR-0038 |
| [#56](https://github.com/RobotSe7en/ayni/pull/56) | 2026-10-24 | `MA-006` F 收尾 | 统一 Native 验证、覆盖率补测、资源量测及状态/契约收尾 | ADR-0038 |
| [#60](https://github.com/RobotSe7en/ayni/pull/60) | 2026-10-25 | `MA-018A` 资源机制 | 资源目录、健康查询、公平全组准入及停止代际隔离，ABI 0.29 | ADR-0042 |
| [#61](https://github.com/RobotSe7en/ayni/pull/61) | 2026-10-25 | `MA-018A` Task 限制 | 持久计数/上限/独立暂停、所有者提高上限与精确命令，ABI 0.30 | ADR-0043 |
| [#62](https://github.com/RobotSe7en/ayni/pull/62) | 2026-10-25 | `MA-008A` 结果交接 | 有界预留、独立取消/终态、非破坏读取与消费确认；内部机制 | ADR-0044 |
| [#63](https://github.com/RobotSe7en/ayni/pull/63) | 2026-10-25 | `MA-008A` 调用前置 | 内部动作状态与编码、typed 输入及静态调用契约；尚未装配调用链 | ADR-0045 |

| [#64](https://github.com/RobotSe7en/ayni/pull/64) | 2026-10-25 | `MA-008A` 公开调用 | 可信目录、稳定调用、持久派发、可靠完成、所有者对账及等待恢复，ABI 0.31 | ADR-0045 |
| [#65](https://github.com/RobotSe7en/ayni/pull/65) | 2026-10-25 | `MA-018A` 可信拓扑 | Device/Component、多对多资源关联与冲突域，ABI 0.32 | ADR-0046 |
| [#66](https://github.com/RobotSe7en/ayni/pull/66) | 2026-10-25 | `MA-018A` 工作策略 | 可信工作分类、后台有限等待/频率和资源队列摘要，ABI 0.33 | ADR-0047 |

| [#67](https://github.com/RobotSe7en/ayni/pull/67) | 2026-10-25 | `MA-008A` 可复用 Mock | A/B 文本与动作、取消/容量与正常关闭，ABI 0.33 不变 | ADR-0048 |

| [#68](https://github.com/RobotSe7en/ayni/pull/68) | 2026-10-25 | `MA-018B` 执行端许可门 | 固定内存/独立 C99、nonce 与包络/样本校验、停止确认分离，ABI 0.34 | ADR-0049 |

| [#69](https://github.com/RobotSe7en/ayni/pull/69) | 2026-10-25 | `MA-018B` 受管理执行闭环 | Core 签发/续租、持久 Scene、最小 Observation、停止事实与资源隔离、Mock 执行器，ABI 0.35 | ADR-0050 |

| [#70](https://github.com/RobotSe7en/ayni/pull/70) | 2026-10-25 | 架构整改（审查基线 `e089a22`） | 依赖检查覆盖根目录、存储清单单一出处、外呼作用域、测试探针、意图 traits、记录登记与存储契约、`ExecutionControl`、到期搜索按事务失效、显式上下文、统一目录与 ABI 闸门；行为、ABI 0.35 与 schema 2 不变 | ADR-0051、ADR-0052（提议） |
| [#71](https://github.com/RobotSe7en/ayni/pull/71) | 2026-10-26 | 异步执行端端口 | 执行端端口改为请求，回答经执行端邮箱由任意线程投递，停止链按异步回答重新论证，Mock 可延迟或丢弃回答，ABI 0.36 | ADR-0052 |
| [#72](https://github.com/RobotSe7en/ayni/pull/72) | 2026-10-26 | P0 结构收尾 | 主版本 0 只收当前 ABI 布局、泛型精确命令查找、调用对 Task 的传播移到 work、组合根共享默认配置、文档/草案对齐；两个 runner 并行跑额外配置；ABI 0.36 布局与 schema 2 不变 | ADR-0053、ADR-0042 补充决定 |
| [#73](https://github.com/RobotSe7en/ayni/pull/73) | 2026-10-26 | P1 typed 工具调用（`MA-008C` 前半） | 模型结果可为最终文本或至多 8 项 typed 调用；GoalType 允许列表 ∩ 已授权的工具视图在 submit 内交给 Provider；post 时校验并编码、回执时按视图复核；`MODEL_OUTPUT` 原因；列表随 type21/v4 持久；策略经 `call_tool` 读取并自行请求；Mock tools 服务；ABI 0.37、schema 2 不变 | ADR-0054 |

| [#75](https://github.com/RobotSe7en/ayni/pull/75) | 2026-10-26 | P1 OpenAI-compatible Provider（`MA-008C` 后半） | Native libcurl multi、yyjson 有界解析、typed 工具解码、本地协议夹具与手动 DeepSeek 文本/工具验证；默认 model.slot 准入，无自动重试，取消未知；ABI 0.37/schema 2 不变 | ADR-0055 |
| [#76](https://github.com/RobotSe7en/ayni/pull/76) | 2026-10-26 | P1 双策略（`MA-009`） | 有限规则、顺序 ToolLoop、摘要与整段对话值删除、旧 writer 兼容声明、已完成 durable 检查点导出；ABI 0.38，schema 2 不变；文件存储随 P2 | ADR-0056 |

| [#77](https://github.com/RobotSe7en/ayni/pull/77)、[#78](https://github.com/RobotSe7en/ayni/pull/78) | 2026-10-26 | P2（MA-020 前半） | 双 Agent Native/Sim runtime、有界查询、对话文件与单进程 CLI；正常功能、独立审查及 ASan/UBSan CI 通过；ABI 0.39、schema 2 | ADR-0057 |

| [#79](https://github.com/RobotSe7en/ayni/pull/79) | 2026-10-27 | P3 F01/F14/F08 | Kernel 私有状态、service/runtime/ports、StepContext 与回调守卫 | ADR-0058 |
| [#80](https://github.com/RobotSe7en/ayni/pull/80) | 2026-10-27 | P3 F04/F12 | Grant 历史与拒绝闸门、Grant/命令 PersistentStore | ADR-0059 |
| [#82](https://github.com/RobotSe7en/ayni/pull/82) | 2026-10-27 | P4 MA-011B | 可信 UTC、POSIX 日历、稳定补发/重新授权游标；ABI 0.41 | ADR-0061 |
| [#83](https://github.com/RobotSe7en/ayni/pull/83) | 2026-10-27 | P4 MA-008B 入口 | 任意线程观测 FIFO、可靠 gap、Mock 媒体；ABI 0.42 | ADR-0062 |
| [#84](https://github.com/RobotSe7en/ayni/pull/84) | 2026-10-27 | P4 MA-008B 订阅 | Task 所有订阅、有限租期/自动续租、新代际与 WAITING；ABI 0.43 | ADR-0063 |

| [#86](https://github.com/RobotSe7en/ayni/pull/86) | 2026-10-27 | P5 F04 前置整改 | Agent/Goal/Task PersistentStore、多记录快照游标及完整对象预算，ABI 0.43/schema 2 不变 | ADR-0064 |
| [#87](https://github.com/RobotSe7en/ayni/pull/87) | 2026-10-27 | P5 MA-019 通用机制 | 持久暂停组、不可变成员/祖先限制、不补积压与旧输入前沿、在途停止锁存，ABI 0.44/schema 2 | ADR-0065 |
| [#88](https://github.com/RobotSe7en/ayni/pull/88) | 2026-10-27 | P5 MA-019 应用 | 统一路由、四类目标、异步通知及安静时段、并行状态与调用 effect，ABI 0.45/schema 2 | ADR-0066 |
| [#89](https://github.com/RobotSe7en/ayni/pull/89) | 2026-10-27 | P5 MA-020 SDK/示例 | C++17 生命周期封装、四个公开示例与双 Agent Mock 脚本；ABI/schema 不变 | ADR-0067 |
| [#90](https://github.com/RobotSe7en/ayni/pull/90) | 2026-10-27 | P6 正常集成 | 编译 JSON profile、SimNode 正常子场景、脱敏可重放轨迹；ABI/schema 不变 | ADR-0068 |
| [#91](https://github.com/RobotSe7en/ayni/pull/91) | 2026-10-27 | P6 F18 Native 基线 | 最大配置、选定满表负载、编码/管理提交/step 分开计时与容量边界；ABI/schema 不变 | ADR-0069 |
| [#92](https://github.com/RobotSe7en/ayni/pull/92) | 2026-10-27 | P6 MA-038 正常子集 | 独立 Kernel/时钟/存储、typed 远程调用/查询、固定服务 Task 与在途正常关闭；完整协议仍未完成 | ADR-0070 |
| [#93](https://github.com/RobotSe7en/ayni/pull/93) | 2026-10-27 | P6 正常收尾 | 统一覆盖率、配置证据与正常集成报告，安全覆盖差距保持开放；ABI/schema 不变 | ADR-0071 |
| [#94](https://github.com/RobotSe7en/ayni/pull/94) | 2026-10-27 | MA-019/MA-020 修订 | 单一 ToolLoop 助理、模型计划工具（待批、按 operation_id 找回、待批至多 4 个、停后不登记）、确定性停止；AYNF v3/参数 format2，旧目录拒绝；ABI/schema 不变 | ADR-0072 |
| [#95](https://github.com/RobotSe7en/ayni/pull/95) | 2026-10-28 | MA-019 修订 | 每次模型请求写明当前本地日期、星期与时区，模型自行换算相对日期；CLI `timezone` 配置或本机时区；指令（含时间）作为 system 消息、对话作为 user 消息；Core/ABI 布局/schema 不变 | ADR-0073、ADR-0074 |

| [#96](https://github.com/RobotSe7en/ayni/pull/96) | 2026-10-28 | 完整实验阶段 0 | 机制/证据缺口盘点，恢复本轮完整 E2→E1→E3→E4 所需故障/恢复专项，逐阶段审查/CI/自动合并；完整 E1 保留 ≤10 step，变异继续暂停 | — |

| [#97](https://github.com/RobotSe7en/ayni/pull/97) | 2026-10-28 | 完整实验阶段 1 | 编译完整 profile、受控切断/重建、独立外部效果与 HARNESS 自验证，首错/清理/产物独立失败 | ADR-0075 |

| [#98](https://github.com/RobotSe7en/ayni/pull/98) | 2026-10-28 | 完整实验阶段 2 / MA-022 | E2 F0～F14、真实提交边界与独立执行端证据；修复满盘关闭预算缓存死锁；ABI/schema 不变 | ADR-0076 |
| [#99](https://github.com/RobotSe7en/ayni/pull/99) | 2026-10-28 | MA-021 完整 E1 | 共享服务步骤 1～11、来源失效恢复与可重放证据；交付结果见 PR | ADR-0077 |
| [#100](https://github.com/RobotSe7en/ayni/pull/100) | 2026-10-28 | MA-023 完整 E3 | 双 Mock/规则与真实 ToolLoop、共同检查点、旧调用隔离与不兼容暂停；交付结果见 PR | ADR-0078 |
| [#101](https://github.com/RobotSe7en/ayni/pull/101) | 2026-10-28 | MA-033 完整 E4 | Native/Sim 父子组合、11 个提交边界、依赖/停止、版本矩阵与通知；typed/开发期存储例外见 ADR | ADR-0079 |
| [#102](https://github.com/RobotSe7en/ayni/pull/102) | 2026-10-28 | 安全覆盖收尾 | 原保守范围补测达到本轮用户接受的 ≥80%；修复订阅新代恢复与回调 ABI 尺寸分类，通用 90% 偏差保留 | ADR-0080 |
| [#103](https://github.com/RobotSe7en/ayni/pull/103) | 2026-10-28 | Core Alpha Native 报告 | 汇总完整实验、SDK、最终配置和覆盖证据，保留兼容待决与平台边界；本轮结束，M2 暂缓 | ADR-0080 |
| [#104](https://github.com/RobotSe7en/ayni/pull/104) | 2026-10-29 | 通用 AgentOS 架构规划 | 本机优先的能力、发现和 MCP/MHS/ROS 接入与代码处置计划 | ADR-0081 |
| [#105](https://github.com/RobotSe7en/ayni/pull/105) | 2026-10-29 | AP-00 入口清理 | 删除已替代草案和旧路径，消费者正式入口；ABI0.46 | ADR-0082 |
| [#106](https://github.com/RobotSe7en/ayni/pull/106) | 2026-10-29 | AP-00 当前存储统一 | schema3 完整记录、严格未知拒绝及必要恢复验证；ABI不变 | ADR-0082 |
| [#107](https://github.com/RobotSe7en/ayni/pull/107) | 2026-10-29 | AP-01 能力自描述 | C99 说明端口、exact 配对复制、真实模型 codec、本机 schedule 与模拟设备；ABI0.47/schema3 | ADR-0082 |
| [#108](https://github.com/RobotSe7en/ayni/pull/108) | 2026-10-29 | AP-00/AP-01 审查扫尾 | 同步版本/授权文档，显式记录证明、移除存储残留、前移说明端口形状校验并保留无工具请求；ABI 0.47 / schema 3 不变 | ADR-0082 |
| [#109](https://github.com/RobotSe7en/ayni/pull/109) | 2026-10-29 | 普通命令回执修复 | 统一校验 ordinary mapping 的 effect，修正输入边界夹具及当前 schema 注释；ABI 0.47 / schema 3 不变 | ADR-0082 |
| [#110](https://github.com/RobotSe7en/ayni/pull/110) | 2026-10-29 | 通用基础与实验路线 | 通用 AgentOS 无必备设备集合，基础先于独立平台实验；同步规划、契约适用范围与交接 | ADR-0083 |
| [#111](https://github.com/RobotSe7en/ayni/pull/111) | 2026-10-29 | AP-02 盘点与设计 | 完成实际消费者、类型/授权/持久设计及独立审查修订；记录用户选择，生产实现尚未开始 | ADR-0084 |
| [#112](https://github.com/RobotSe7en/ayni/pull/112) | 2026-10-30 | AP-02 业务契约与原证明 | 统一有界 Schema/规范值、完整授权证明、结构结果和全仓消费者；Shell Provider；ABI0.48/schema4 | ADR-0084 |
| [#113](https://github.com/RobotSe7en/ayni/pull/113) | 2026-10-30 | AP-03 按需绑定与目录 | 配置期说明采纳、Task 分页目录、首次完整 Grant 绑定、独立暂停及显式重授权；ABI0.49/schema5 | ADR-0085 |
| [#114](https://github.com/RobotSe7en/ayni/pull/114) | 2026-10-30 | AP-03 Native harness | 固定四工具、标准消息、披露来源、token 控制、SQLite 历史/原始结果及显式 Shell；ABI0.50/schema6 | ADR-0085 |

## 工作包日期

| 工作包 | 开始 | 结束 |
|---|---|---|
| `MA-003` | 2026-10-19 | 2026-10-19 |
| `MA-005` | 2026-10-19 | 2026-10-19 |
| `MA-004` | 2026-10-19 | 2026-10-20 |
| `MA-010` | 2026-10-20 | 2026-10-21 |
| `MA-006` | 2026-10-22 | 2026-10-24（授权机制及模拟消费者；真实消费链随后续包） |

| `MA-009` | 2026-10-26 | 2026-10-26（正常双策略/对话值；文件存储随 P2） |
| `MA-019`、`MA-020` | 2026-10-27 | 2026-10-27（P5 正常功能；完整实验另行验收） |

## 交接状态（2026-10-30）

AP-02 已由 [#112](https://github.com/RobotSe7en/ayni/pull/112)、ADR-0084 交付（ABI 0.48/schema 4）；AP-03 第一部分由 [#113](https://github.com/RobotSe7en/ayni/pull/113)、ADR-0085 交付配置期说明采纳、Task 目录/按需绑定（ABI 0.49/schema 5），第二部分已实现固定模型工具、连续上下文、SQLite 和显式 Shell（ABI 0.50/schema 6），正常验证与独立复审见 [#114](https://github.com/RobotSe7en/ayni/pull/114)，基础通用能力未完成。此前规划/设计见 #110/#111，M1 Native/Sim 与历史覆盖偏差见 [Core Alpha Native 报告](m1-core-alpha.md)，不代表平台实验或硬件验收。

- **已交付：** Native/Sim 的生命周期、工作、授权、资源、调用、记忆/日历、助理应用、SDK 和能力自描述；完整 MA-038 仅有 typed 正常子集，ESP-IDF 存储和实机仍未验证。各包范围及证据见上方 PR 表。
- **用户最新决定：** Ayni 是通用原生 AgentOS，宿主不要求语音、视觉、运动或特定设备。先完成基础通用能力，再独立开展 ESP32-S3、Linux 系统等实验；两平台不互为前置。详见 [当前实施与实验计划](../../docs/architecture/foundation-and-experiment-plan.md)及 ADR-0083；原 M2～M6 设备产品链不再作为所有部署的必经路线。
- **本轮与下一步：** AP-02 统一业务 Schema/规范请求与结果、完整授权证明、存储及全仓消费者；AP-03 第一部分实现目录/按需绑定，第二部分固定工具、标准消息/token 预算、SQLite 及 Shell 部署验证见 #114；接下来按 AP-05/AP-06 与基础收尾推进。相关正常回归、设计自查、独立审查修订及 CI 记录集中在 PR。
- **实验与可选能力：** Native/Sim 日常测试继续；基础门槛通过后再安排 ESP32-S3、Linux 实际 OS 集成及其他平台实验。语音/视觉、运动与硬件停止、MCP/ROS/MHS、peer 和轮式助理试用按需选择；启用的物理执行保证、OTA/硬件/耐久证据不能省略或用模拟替代。
- **已定与待决：** AP-02 首版含有界嵌套/列表及浮点/精确十进制；列表同一完整授权逐项覆盖、拒空，权限只用精确数值；本包直接升级并拒旧保留目录，无 reader/迁移器（ADR-0084）。固定带 ayni_ 前缀的四工具、当前授权目录披露、完整 Shell 和标准消息/上下文预算已定（ADR-0085）；部分授权工具已选定按需绑定；后续包控制、兼容和平台指标仍分别决定。真实模型多轮试用、长期密钥配置和完整 MA-038 旧待决保留；历史 ≥80% 接受不替代通用 90% 目标。
- **工作流与边界：** 依相关正常测试、设计自查、适用独立审查及 CI 交付；具体命令、结果与运行编号集中在 PR。本包不启动平台/硬件、付费 API 或新专项；变异及新增故障/恢复专项仍按 workflow 现行授权处理。

## 剩余工作

### `MA-006` 六部分（用户已批准，2026-10-22）

每部分单独 PR、独立验证/审查并按依赖顺序合并；实际交付状态见下表最后一列。

| 部分 | ADR / ABI | 内容 | 状态 |
|---|---|---|---|
| A | 0028 / 0.16 | Host 受管主体/会话入口、owner/revision 持久绑定、失配受限、旧恢复工作 `LEGACY_UNCONFIRMED` 禁行 | 已合并 #25（`f297941`）；验证与独立审查通过 |
| B | 0029 / 0.17 | Capability 有界注册、类型/单位/取值域与范围规范化、覆盖/收窄 | 已由 #26 合并（`00a4126`）；Native 五配置、独立审查、35 项变异及最终八项 CI 通过 |
| C | 0030 / 0.18 | Grant 创建/查询/期限/委派/安全回收，规范请求去重与持久恢复 | 已合并 #27，验证见 ADR-0030 和 PR |
| D | 0031 / 0.19 | 即时拒绝、待决撤销、阶段回执、背压与自动重载保护 | 已合并 #28，验证见 ADR-0031 和 PR |
| E | 0037 / 0.20～0.23 | GoalType 需求、`PROPOSED`、专属 Grant 与原子批准、激活后计时及最低门控 | 已由 #42～#45、#47 合并；验证和覆盖率见 ADR-0037 |
| F | 0038 / 0.24～0.27 | Task/子目标准入、决策复核、显式恢复、撤销清理、新格式正常重启 | F1～F5 已由 #49、#51～#55 交付；统一验收及本轮覆盖率门槛见 ADR-0038 |

- **A 的历史交付边界（旧格式恢复/首次收养规则自 ADR-0038 起由 schema 2 完整事实要求替代）：** `ayni_register_principal`/`ayni_open_session` 声明移至 Host 受管头文件；Core 不认证密码/TLS。持久 OwnerBinding 与本次 Host 配置不匹配则在恢复写入前只读受限，不转移所有权、不丢命令窗口。首次绑定持久前不开放敏感会话。旧日志无绑定时先核对全部保留 USER Goal origin 的 principal/revision 及 CommandMapping principal，任一冲突即无新写地受限；证据全一致或完全无所有者证据才允许可信 Host 首次绑定，已有绑定以其为权威。这不实现转移，旧工作仍禁行，A 无批准接口。
- **B 的实施边界：** 新类型与注册入口在 `authority.h`；`catalog/capability` 保存 ID/版本、字段类型/用途/单位及有限域，`authority/scope` 提供规范化、覆盖/收窄与实际请求完整性。字段最多 8、集合原始输入最多 8；完整注册域也必须适配 64–4096 B 的配置上限。新增配置默认 Capability 16、Scope 256 B；目录采用 metadata 加字节 stride，默认/最小工作内存本地量测为 67,544/29,060 B。B 不开放 Grant/审批/Provider，不新增持久记录，不把 Scope 包含成立当作授权通过；详见 ADR-0029。
- **C 的实施边界：** 管理端 Session+非零 command 创建/委派；固定 typed Grant/字段查询；child 校验 parent revision，depth 根 0 按边数；未持久 parent BUSY/GRANT_NOT_DURABLE。Grant 保存完整源域+展开 Scope，恢复 exact 域变化只 hold；命令保存原始规范请求并确切比较，旧目录/主体/父失效后先返回已映射结果。限时只存 duration，boot 从 ID 得到，deadline 仅内存且恢复不重锚；持久终态且无保留 child 的叶节点按终态提交/ID 让位。known Scene/Quota UNSUPPORTED，撤销/Goal 关联分别由 D/E 交付（C历史边界；D的Gate/回执/Grant清理现已实现并验证，见ADR-0031），C 最终五配置、本地独立验证及代码 CI 通过，已合并 #27（`b133032`）（ADR-0030）。
- **D 的实施边界：** authority拥有独立Gate与根/后代持久清理；command拥有每Grant专用pending、原16 B canonical、同一窗口与易失pin；Kernel只编排。all-kind pending优先、跨floor发布、同target另命令BUSY；PENDING effectNONE/rev0/seq0/boot0，APPLIED带本boot/seq，DURABLE不能用跨reload公开floor推断。隔离/读取/绑定失败持续拒绝，完整恢复原canonical或重提，不二次CAS；full-process PENDING不承诺。独立cleanup_stamp保留终态顺序，回收需最新清理持久且无Gate/pending引用。snapshot13/v2兼容v1，commandv3与15/16/14v2按上下文恢复（ADR-0031）。E 已交付 Goal 审批与最低消费门控；完整工作传播与恢复仍由 F 完成。
- **13 项产品语义：** 缺授权合法目标保存 `PROPOSED`；批准只覆盖原目标参数/重复规则；待批不积累 occurrence、批准后开始延时/周期；撤销保留进度并须显式恢复；长期或当前 boot 限时授权；有限声明式约束；旧工作保留并待确认（本轮 F2 起按 ADR-0038 开发期不兼容规则替代）；撤销写失败保持拒绝并沿用隔离重载；统一 `PENDING`/`APPLIED`/`DURABLE` 回执；所有权完整转移随 `MA-028`；额度随 `MA-018`；Scene `UNSUPPORTED`；Grant 满时仅回收最早安全终态，无候选则拒绝。完整边界与失败语义见 ADR-0028。
- **验收归属：** 本包只验证授权机制及模拟消费者，F 已交付机制与模拟消费者闭环；真实屏障后复核/停止续租随 `MA-018`，在途取消/外发随 `MA-008`，完整撤销故障矩阵随 `MA-022`。物理停止仍需执行端与硬件证据。
- **当前恢复场景：** `WorkAuthorityRestart.permanent_admission_continues_the_same_task_checkpoint_and_steps`、`WorkAuthorityRestart.old_boot_timed_authority_requires_explicit_reauthorization` 及 owner 失配/缺失证据场景验证正常继续与安全拒绝；原 LEGACY_UNCONFIRMED 场景已由 F 的新格式规则替代，历史交付记录不作为当前兼容承诺。
- **证据入口：** A–D 的仓库内测试与契约见 ADR-0028～0031、`contracts/traceability.md`；构建/变异结果见 #25～#28，R1/R2/R3 结果见各 PR。历史覆盖率及故障原始日志保存在本机，未作为仓库或 CI 附件交付，不能独立复核其原始记录；不以这些历史数字替代本轮验证。
- **C 最终证据与限制：** GCC Debug/Release/ASan+UBSan/TSan、Clang Debug各 **784/784**，模拟器退出0，全部177个C/C++文件格式检查通过。50项独立变异 **49检出/1等价(C20)**，C18/C31/C43/C46补测后原选点重跑，C29测试helper失败即止后正常检出；275文件/11个production targets恢复核对。最终全TU覆盖Core含头行 **6684/6856=97.49%**、分支 **4570/5041=90.66%**；C组件分支 **985/1024=96.19%**，改动可执行行分支 **1208/1303=92.71%**，防御分支偏差见ADR-0030。默认/最小arena **106,520/31,072 B**，较B **+38,976/+2,012 B**、对齐8B，strictC99独立复核一致。两项P2已修复并独立回看。旧779与格式前784结果分别归档；最终源码含两文件纯格式修正，最终计数重新清gcda采集，前后175源码/测试文件SHA一致。原始记录 `../ayni-ma006-evidence/c/`；未据Native数字声称硬件RAM/栈、存储耐久或物理停止已验证。


### `MA-010` 之后的恢复验收边界

- E+F 已在 #21 完成当前工作模型的重试、栅栏、快照、轮转、限频和 schema 0→1 迁移；旧批次大于本次缓冲的交接问题已通过 `READ_BUFFER_TOO_SMALL` 和严格回包校验修复。默认三倍缓冲避免一个最大批次在途造成的分段问题，显式小缓冲仍允许并按契约背压。
- 输出门控（`requires_seq`）随产生对外输出的包实现；当前故障路径不能证明隔离或恢复时只读受限，不发出新输出。在途动作并存、动作账本/Grant/预算/记忆记录的恢复与完整 E2/E4，仍由各所属包验收。
- ESP-IDF Host 尚无存储端口，`ayni_start` 返回 `UNSUPPORTED`；真实 Flash 擦写和执行器调度随 `MA-026`，真实 OTA/回滚随 M4。模拟旧 reader 的拒绝不等同于旧固件二进制或硬件回滚实测。

### 第 3 至 6 步

- 每个包开始时先按 `docs/workflow.md` 第 2 节读计划 §9.1、契约与 ADR，再拆分；拆分与决定写进该包的第一个 ADR 和本文。
- ADR-0025 决策 2：调用身份、动作账本、Grant、执行许可、预算、记忆与删除等记录的断电验收，由各记录所属的包用 `MA-010` 的断电注入补上；写入 `UNKNOWN`/`FAILED` 与在途动作并存的验收随 `MA-022`。
- ADR-0025 后果：`MA-008`、`MA-018` 使 Task 进入 `RUNNING`、`CANCELLING` 之后，仍有未结束 Task 的终态 Goal 不得让位，由那时的包保证并补测试。
- `MA-019` 负责“暂停助理”作用于哪些目标（ADR-0018 记录的拆分）与日历提醒的“错过补发一次”（计划 §14.1、C-PROD-018）。
- 第 6 步结束时写 Core Alpha 报告，内容按计划 §9 M1 行与 §15：电脑 E1–E4、Linux CI、SDK 与独立 Core 报告。

## 已作的决定

- **历史流程（交付规则已由 2026-10-24 最新用户决定及 workflow 替代）**：每个工作包至少一个 PR；用户 2026-10-20 要求提速后，`MA-010` 剩余部分合为 B+C+D 与 E+F 两个 PR，2026-10-22 起改回每个部分一个 PR；植入缺陷测试与独立审查只对核心逻辑做，缺陷由另一个会话出题，审查另查四项可读性问题（`docs/workflow.md` 第 2 节）。M1 的 PR 在 CI 全绿且审查发现处理完后由 Claude 自行合并（用户授权给 Claude；其他代理按用户的指示合并）。实现时使用 xhigh 推理强度。
- **交接**：用户 2026-10-21 要求 Claude 做完当前工作后停止，并在决策卡上选了“只做当前 PR”：Claude 在 #20 合并后停止，`MA-010` E+F 起的工作由其他大模型按本文与 `docs/workflow.md` 接手。
- **产品语义**：各包中由用户决定的事项记在对应 ADR 的决策中，标注“用户选定推荐项”或用户的选择；影响计划的写在计划 §14.1。`MA-006` 的 13 项与六 PR 拆分见 ADR-0028（2026-10-22 用户批准实施计划），A/B 已合并，C 已合并 #27（`b133032`）；用户 2026-10-23 已授权继续完成整理与 E/F，尚不表示全包完成。`MA-010` 的见 ADR-0025 决策 2、3 与 ADR-0026（重启后的计时、目录缺失时保留并停下、`durable_seq` 进入步骤报告、按 principal 有界的命令表），以及 ADR-0027 的历史保留 A、异常与维护冻结、未注册所有者命令窗口、共享 10 ms 写频、三倍默认缓冲和不合成请求超时等用户决定。
- **M0 遗留**：开源许可证暂缓决定，仓库私有期间不受影响（计划 §14.2）；本地 API 的形态已由用户决定，IDL 与线格式仍待定（ADR-0009），在 ESP32 实测之后、最迟在 `MA-038` 编写消息解码器之前决定；开发机以 Linux 为准；300 元是目标值，超出须写明理由并由用户确认。

## 需要用户的事项

- 开发板（YD-ESP32-S3 类 DevKitC-1 兼容板，模组 N16R8，`reports/hardware/m0/board-feasibility.md`）插在用户的 Linux 电脑上时，经用户允许：烧录 Hello 固件、确认实际 Flash 与 PSRAM 容量、读取只读的 eFuse 摘要。不执行任何 burn 类命令。
- 许可证在首次公开发布前决定。
