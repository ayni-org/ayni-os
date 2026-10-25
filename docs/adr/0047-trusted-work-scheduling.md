# ADR-0047：可信工作分类与资源等待策略

- **状态**：实施中；后台等待与默认频率按推荐项实现，改选再改
- **日期**：2026-10-25
- **关联**：MA-018A、MA-008A；ADR-0042、0045、0046；C-RES-004、005、009；C-RUN-001；计划 §9.1

## 背景

#65 已交付可信资源拓扑和冲突域，公开调用仍统一使用普通优先级及 30 秒暂停。资源模块已有优先级和 wait/pause/skip 机制，但没有可信业务分类的消费者，提醒可能被普通等待上限暂停，后台周期也不能按计划跳过。

## 决策

1. GoalType 注册附带有限 WorkProfile：ordinary、foreground、reminder、background。目录持有规范配置，Goal 保存创建时的分类及后台最短周期；策略、模型参数和调用请求都不能覆盖它。CallRuntime 仅将已存分类映射至现有资源队列，不建立第二个调度器。队列复制查询复用资源属主现有峰值，给出容量、等待/持有组数和当前或已结束的最长等待；不另存一套统计。
2. foreground/reminder 使用前台优先级，ordinary 使用普通，background 使用后台；同级仍按 Agent 轮转、同 Agent FIFO。已准入工作不被强抢，独立停止仍绕过普通队列。
3. foreground/ordinary 等待资源 30 秒后保留原调用并暂停，所有者显式恢复规则仍按 ADR-0045。reminder 的资源等待无期限，GoalType 同时必须声明派生 Task 的 occurrence 等待无期限；仍受显式取消、授权和 Goal 到期约束。
4. **后台等待（产品语义，推荐项待用户回复）**：最多等资源 30 秒，未准入则结束本次 Task 为 EXPIRED、调用为 Core 专用 SKIPPED，带 EXPIRED/RESOURCE_WAIT 及明确未外发事实。策略不能把本次重新派发；后续周期继续，调用身份、步骤占用与已接受模型次数保留。Goal 的 occurrences_skipped 仍只统计尚未派生 Task 的跳过，不把已派生 Task 混入该计数；后者由 Task/Call 事实查询。
5. **后台频率（产品语义，推荐项待用户回复）**：默认最短周期 5 分钟，可信启动配置可显式改变，不能短于系统的 1 秒周期下限。background GoalType 仅允许 INTERVAL，派生 Task 前的 occurrence 等待须有限且至多 30 秒；创建请求可选更慢周期，不能提高频率。首次可立即运行。此限制针对每个 Goal 的周期，不是费用额度或 Agent 总调用配额。
6. ABI 0.33 发布分类、查询及 SKIPPED 诊断。非默认 Goal profile 使用 REQUIRED type3/v5（journal）和 v6（snapshot），保存 12 字节类型/周期下限；默认 profile 保留旧编码。SKIPPED 使用 REQUIRED type21/v2，普通调用保持 v1。schema 2 不变，旧 reader 明确拒绝新必需版本并保留旧目录。
7. 重启重新匹配 GoalType ID/version 和保存的 WorkProfile；不一致保留检查点并暂停，不静默改变旧 Goal 的优先级/周期下限。修改 profile 使用新 GoalType 版本；本轮不建立运行中重分类或迁移接口。

## 后果

应用新增提醒/反思类型只需在 runtime 提供可信 GoalType profile；资源、授权、调用账本和任务生命周期保持各自属主。后台未外发的等待超时不制造 RESULT_UNKNOWN；Provider 无权回送 SKIPPED。普通 Task 的原超时恢复语义不变。

## 备选方案

让模型每次提交优先级会绕过后台限制；根据 GoalType 名称猜测类别不可验证。只在易失调用槽保存分类，会让重启后的排队语义取决于新配置而失去原创建事实。累积费用预算不在本轮范围。

## 验证

实施与正常集成验证进行中。计划覆盖优先级、提醒持续等待、后台跳过/后续周期、可信频率拒绝及诊断；不新增故障、重启、恢复或变异专项，不据此报告完整 M1。
