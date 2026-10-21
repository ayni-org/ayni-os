# 架构决策记录（ADR）

本目录记录 Ayni 的重大架构决策。规范性规则以实施计划 [`docs/ayni_v1.0.md`](../ayni_v1.0.md) 的对应章节为准；ADR 记录背景、决策、取舍、备选方案与验证方式，不重复全部规则，避免多处维护同一条款。

## 状态

- **提议**：候选方案，须在记录注明的时点前接受、修订或否决；未决定前不得写成既定设计。
- **已接受**：作为设计基线写入实施计划；不代表已实现或已验证，实现与验证证据另见 `reports/`。
- **已取代**：由后续 ADR 取代；修改已接受决策的语义时新增 ADR，不改写历史记录。

## 索引

| 编号 | 标题 | 状态 | 计划章节 |
|---|---|---|---|
| [0001](0001-device-governance-and-execution-permits.md) | 设备治理模型与执行许可 | 已接受 | §2.2、§2.6、§2.10、§7.3 |
| [0002](0002-commit-and-output-gating.md) | 提交与输出门控协议 | 已接受 | §2.4、§2.8、§4.3 |
| [0003](0003-trust-model-and-adapter-confinement.md) | 信任模型与适配器约束 | 已接受 | §2.5、§3.7 |
| [0004](0004-fabric-identity-freshness-idempotency.md) | 跨设备身份、新鲜度与幂等 | 已接受 | §3.1、§3.4 |
| [0005](0005-esp32-layered-stop-chain.md) | ESP32 分层停止链 | 已接受 | §4.1、§7.1、§7.3 |
| [0006](0006-kernel-lifecycle-ports-and-policy-interface.md) | Kernel 生命周期、Host/Provider 边界与策略接口 | 已接受 | §2.0、§2.5、§3.2、§4.4 |
| [0007](0007-notification-presentation-and-task-state.md) | 通知呈现复用能力调用与 Task 状态维度 | 已接受 | §2.4、§5.4 |
| [0008](0008-core-modules-by-invariant-owner.md) | Core 模块按不变量属主划分（两级目录，2026-10-19 修订） | 已接受 | §2.0、§2.3、§8 |
| [0009](0009-idl-and-local-api-encoding.md) | IDL 与本地 API 编码 | 提议（本地 API 形态已于 2026-10-19 接受） | §3.1、§3.2 |
| [0010](0010-composition-root-and-targets.md) | 组合根 `runtime/` 与可执行目标 `targets/` | 已接受 | §2.6、§8、§11 |
| [0011](0011-build-layout-and-dependency-checks.md) | 构建布局与依赖检查 | 已接受 | §2.0、§2.3、§8、§11 |
| [0012](0012-test-framework.md) | 测试框架 | 已接受 | §10.1、§10.6 |
| [0013](0013-contract-format-before-idl.md) | IDL 选定之前的契约表达方式 | 已接受 | §3.1、§3.2 |
| [0014](0014-core-does-not-allocate.md) | Core 不自行分配内存 | 已接受 | §3.2、§4.2、§8 |
| [0015](0015-ingress-lock-and-completion-slots.md) | 入口临界区、有界事件队列与完成槽 | 已接受 | §3.2、§4.2 |
| [0016](0016-boot-recovery-and-bounded-step.md) | 启动恢复、持久 boot_epoch 与有界 step | 已接受 | §3.2、§4.2、§4.3 |
| [0017](0017-work-model-principals-agents-goals.md) | 工作模型第一部分：主体与会话、Agent 实例、GoalType 与 Goal 意图 | 已接受 | §2.2、§2.4、§3.2、§3.6、§5.3 |
| [0018](0018-tasks-and-propagation.md) | 工作模型第二部分（一）：Task、派生与传播 | 已接受 | §2.2、§2.4、§3.2、§4.2、§5.3、§9.1 |
| [0019](0019-time-triggers.md) | 工作模型第二部分（二）：延时与周期触发 | 已接受 | §2.1、§2.2、§4.4、§5.3、§14.1 |
| [0020](0020-goal-expiry-and-bounded-waits.md) | 工作模型第二部分（三）：Goal 失效与有界等待 | 已接受 | §2.2、§2.4、§4.4、§5.3 |
| [0021](0021-local-input-events.md) | 工作模型第二部分（四）：事件投递与本地输入触发 | 已接受 | §2.1、§2.2、§3.2、§4.2、§5.3 |
| [0022](0022-strategies-and-decisions.md) | 工作模型第三部分（一）：策略与决策 | 已接受 | §2.4、§2.5、§2.7、§3.2、§4.2、§5.3 |
| [0023](0023-goals-end-with-their-last-task-or-occurrence.md) | Goal 随最后一个 Task 的取消或最后一个 occurrence 的跳过结束 | 已接受 | §2.2、§2.4、§5.3 |
| [0024](0024-steps-and-private-checkpoints.md) | 工作模型第三部分（二）：步骤与私有检查点 | 已接受 | §2.4、§2.5、§3.2、§4.2、§5.3 |
| [0025](0025-persistence-split-and-terminal-goals-give-way.md) | 持久化与恢复（一）：拆分与终态 Goal 让位 | 已接受 | §2.8、§4.2、§9.1 |
| [0026](0026-work-journal-replay-native-file-log-and-commands.md) | 持久化与恢复（二）：工作日志、重放、Native 文件存储与命令幂等 | 已接受 | §2.8、§4.2、§4.3、§9.1 |

## 模板

新 ADR 使用下一个编号，并包含以下小节：状态、日期、关联（计划章节、工作包、实验）、背景、决策、后果、备选方案、验证。提议状态的 ADR 另写明决定时点与决定依据。
