# ADR-0072：单一助理、模型计划工具与确定性停止

- **状态**：已接受；实现、验证与独立审查见 [#94](https://github.com/RobotSe7en/ayni/pull/94)
- **日期**：2026-10-27
- **关联**：MA-019、MA-020；ADR-0057、0060、0065、0066；C-NATIVE-001、007、011、012，C-PROD-040、041、050

## 背景

ADR-0057/0066 的 Native 演示装配有两个 Agent：A 运行预设规则，B 运行 ToolLoop；CLI 按 `@a/@b` 点名，未点名时 `check/检查` 给 A、其余给 B。四类长期目标只能由所有者用明确命令创建，模型在对话中无法登记提醒或检查。“停”只是普通文本，要经过模型回合才可能有效。

试用讨论中用户提出：用户只面对一个助理，按关键词把文本分给两个 Agent 既脆弱又没有带来能力；模型应能在对话里把用户的长期要求登记成目标；“停”必须立即、可预期，不能依赖模型理解。

## 决策

1. **用户选择（产品语义，用户选定推荐项，2026-10-27）。** 合并为一个助理：所有自然语言都交给同一个 ToolLoop Agent，删除 `@a/@b`、`check/检查` 路由、预设规则 Agent 及其测试/文档；增加模型可调用的“创建任务”工具，按调用操作去重，新目标默认待所有者批准；“停”等命令走不经模型的确定性停止路径。
2. **用户选择（产品语义，用户选定推荐项，2026-10-27）。** “停”取消当前回答，并暂停自主工作组（检查、观察、整理）；已接受的提醒继续，`/resume-assistant` 恢复后台工作。停止短语是去掉首尾空白和句末 `! . ！ 。` 后整句为“停”“停下”“停止”“stop”（ASCII 不分大小写），`/stop` 等价；更长的句子仍是普通对话。它不是 C-STOP 的独立紧急停止链，也不替代后者。
3. **用户选择（产品语义，用户选定推荐项，2026-10-27）。** 双 Agent 版本留下的数据目录不兼容：外层对话文件升为 AYNF v3，v2 在任何启动写入前以 UNSUPPORTED_VERSION 拒绝，整个目录保持原样，CLI 提示改用新目录；不做迁移。应用参数同时升为 format2，format1 拒绝。C ABI 0.45、storage schema 2 与 Core 均不变。
4. **单一助理的装配。** `runtime/common` 的 Profile 只注册一个实例 `demo_assistant`，对话 GoalType 携带模型、摘要和 `schedule` 工具；规则策略、Mock present 工具及其资源从 Native Profile 移除。`applications/assistant` 删除 `route` 与 `merge_suggestions`；记忆、档案、授权命令不再带 Agent 参数。Core 的多 Agent 机制、Sim 正常夹具（ADR-0068）的两实例 profile 与 E1 证据不变，用户面对的入口只有一个助理。
5. **计划工具。** `applications/assistant` 的 `ScheduleTool` 是本地 TOOL Provider `assistant.schedule`：ACTION、无外部效果声明、NOT_IDEMPOTENT、QUERY_NONE、CANCEL_NONE、60 秒时限，入参是一段文本。文本与所有者 `/remind`、`/check`、`/observe` 共用一套文法（`parse_schedule`/`format_schedule`）；整理需要明确选择记忆来源，只由所有者发起。Provider 回调只把请求复制进 4 个有界槽；`settle` 在 owner 线程 step 外逐个决定：先完成解析、编码、查重等可能失败或分配的步骤，再经 runtime 提供的 `GoalPort` 做唯一的效果（提交 GOAL_CREATE），因此拒绝与失败如实报告 effect NONE。文法错误或容量不足以 COMPLETED 文本回给模型，让它向用户确认后重试；无端口或准入已关闭为 FAILED/NONE；端口 BUSY 时请求保留，回执投递 BUSY 时只重投回执，不重做决定。
6. **去重。** 模型来源的 Goal 在参数 format2 中记录 origin=model 与 Core 分配的 operation_id；同一操作的重试先按 operation_id 查找仍保留的 Goal，找到即回报原 Goal，不再创建。Core 的幂等声明与 dedupe 窗口未被使用，去重由应用自己的持久参数承担。新 Goal 是调用实例名下的顶层 Goal，不挂在随回合结束的对话 Goal 下。
7. **默认待批。** 模型来源的 Goal 额外要求 `consent_capability`（Profile 中为 `assistant.confirm`）：它没有 Provider，也不能用 `/grant` 长期授予，所以 Goal 必然停在 PROPOSED，直到所有者 `/approve` 以 GOAL_APPROVE 绑定该 Goal 专属授权。runtime 只在 Goal 持久后向 CLI 发出一次“助理登记了新目标 N，等待你批准”，正文是规范文法；`/cancel N` 放弃。
8. **模块归属。** `applications/assistant` 拥有文法、参数来源、授权要求、计划工具与按操作查找；`runtime/native_node` 拥有管理会话提交（GoalPort）、待公告列表、停止编排（取消当前回合 + 设置暂停组）；`targets/native_cli` 只做命令映射与中文呈现；Core 不新增业务分支。换成语音入口时只需在新入口复用 `stop_request` 与同一运行时操作；换模型 Provider 时计划工具与授权链不变。

## 后果

- 删除的产品行为：规则 Agent A、`@a/@b`、`check` 路由、Mock present 工具、建议合并 API；旧目录须换新目录使用。
- 模型可以提出长期工作，但运行前总要所有者一次明确批准；自然语言日期由模型换算成文法，时区须是 POSIX TZ（中国为 `CST-8`），无法确定时由模型询问用户。
- QUERY_NONE 的代价：崩溃时仍在途的工具调用保持 UNKNOWN，不自动重试；所有者在 `/goals` 看到可能已登记的待批 Goal，用 `/approve` 或 `/cancel` 处理。若用权威查询，Goal 退役后会被误报为“未受理”，故不采用。
- 停止只作用于本助理的当前回合与自主组；Kernel 中其他调用方创建的工作不受影响。计划工具不可取消，停止前已发出的工具调用仍可能登记一个目标，但它同样停在待批，不会自行运行。

## 备选方案

- 保留两个 Agent、改用模型路由：仍需维护两套授权与对话，且多一次模型调用；拒绝。
- 工具直接创建可运行目标或依靠长期授权：模型可在无人确认时新增自主工作；拒绝。
- 依赖 Core 幂等/去重窗口：Core 当前不使用该声明，且窗口外重试会重复；改用持久 operation_id。
- 把“停”交给模型或只做 CLI 本地判定：延迟与不确定性不可接受，且其他入口无法复用；判定放在应用层、编排放在 runtime。
- 迁移双 Agent 目录：用户选择不兼容并明确报错。

## 验证

正常场景覆盖：`check` 是普通对话回合；模型计划工具登记的 Goal 停在待批、经批准后运行，且同一操作不重复登记；文法与停止短语的接受/拒绝边界；停止取消回合、暂停自主组并保留提醒，且不被未完成的对话保存阻塞；双 Agent 目录在启动前被拒绝且目录不变；CLI 脚本与助理示例。命令、结果、独立审查与 CI 见 [#94](https://github.com/RobotSe7en/ayni/pull/94)。未开展新的故障/恢复或变异专项；真实模型调用计划工具的多轮效果待用户本机试用。
