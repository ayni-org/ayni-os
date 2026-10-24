# ADR-0043：Task 运行限制状态与所有者提高上限

- **状态**：实施中
- **日期**：2026-10-25
- **关联**：ADR-0042 决策 3/6/8；MA-018A、MA-008A；C-RUN-001～004；计划 §9.1、§14.1

## 背景

资源准入机制已由 #60 交付。Task 需要在 Provider 消费接入前拥有运行次数、软上限和独立暂停；所有者能提高上限，但不得同时解除用户、策略、兼容或授权暂停。已有普通命令仅摘要去重，不足以支持新的管理命令。

## 决策

1. `work/task` 拥有 used/limit/blocked 和固定启动硬上限。计数为已接受模型请求数，不是费用或 token。默认软上限 8，启动硬上限默认/最大 64，可配置 1..64；0/省略取默认，新 Task 取 min(8, hard)。该计数不预留并发请求/步骤槽，其容量分别由对应表控制。
2. blocked 是独立持久 overlay：达到 used==limit 后，已接受调用仍可正常完成；下一次超额请求才暂停。普通 pause/resume 不清零或解除它；所有者提高后只清该 overlay，保留策略 hold、兼容 hold、用户 lifecycle 和独立授权事实。等待 Goal occurrence 的 held_count 与 overlay 一致。
3. ABI 0.30 增加 kind13 `TASK_RAISE_MODEL_LIMIT` 和 Task 查询字段。当前持久绑定的 MANAGEMENT Session（包括重试）、非零 command_id/expected_revision 和 Task ID 是入口要求。仅可严格提高活 Task，不能超过硬上限；revision 饱和拒绝。Task 的检查和状态转换归 TaskTable，Kernel 在同 transition 加入命令。
4. 复用 CommandTable 的完整 canonical 字节、同一窗口和阶段回执；20 B 规范请求只包含 kind、Task CAS、Task ID、新总上限。Session 不进入身份；模型/策略没有此管理接口。`valid_authority_mapping` 更名为 `valid_exact_mapping`，反映其现在也验证运行限制命令。
5. schema 2 兼容扩展，布局见 records.md。保留旧默认事实 0/8/0；非默认状态 REQUIRED type6/v4-v6，管理提高使用独立批次 type6/v7 + type9/v6 精确命令配对并封闭批次；快照保留完整当前事实/历史命令。旧 reader 遇新 REQUIRED 版本拒绝，降配不足同样拒绝，不截断或重置。复用原 Task authority 邻接规则，没有替换现有授权状态机。
6. 本 PR 完整交付的是状态/持久编码及所有者管理能力。内部接受/超限函数是 MA-008 的接入点；本轮没有生产 Provider 消费者，不声明调用计数与 ActionRecord 原子性、超时/字节限制或完整 E1 已实现。没有新增故障、重启或变异专项。

## 后果

增加固定每 Task 状态和 Task 查询 ABI 字段，日志与快照大小计算覆盖 44 B 扩展状态。提供者替换不会改变计数 owner；新增 Host 仅实现现有持久端口，不重复编码。真实调用仍须在统一提交协调内接入计数及稳定调用身份。

## 备选方案

复用策略 hold 会使普通提高无法区分可恢复与不可恢复原因；用 Goal/Kernel 全局计数会把任务限制互相耦合；普通摘要幂等不能证明完整请求相同；另建运行限制持久表会复制 Task 身份与生命周期。均不采用。

## 验证

相关 Native 正常行为、公开管理入口和 codec 测试通过；完整适用 CI 与独立审查进行中，最终结果及 PR 链接在交付时补充。原有恢复测试保留，本轮没有新增恢复专项，不据普通测试声明完整调用消费或硬件行为已验证。
