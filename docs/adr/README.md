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
| [0009](0009-idl-and-local-api-encoding.md) | IDL 与本地 API 编码 | 提议 | §3.1、§3.2 |
| [0010](0010-composition-root-and-targets.md) | 组合根 `runtime/` 与可执行目标 `targets/` | 已接受 | §2.6、§8、§11 |

## 模板

新 ADR 使用下一个编号，并包含以下小节：状态、日期、关联（计划章节、工作包、实验）、背景、决策、后果、备选方案、验证。提议状态的 ADR 另写明决定时点与决定依据。
