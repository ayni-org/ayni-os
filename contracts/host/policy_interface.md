# 策略与 GoalType 接口

- **关联**：计划 §2.5、§2.7、§3.3「新策略」、§5.3；ADR-0006、ADR-0017；`MA-004`、`MA-009`、`MA-019`；E3 步骤 2–5，E4 步骤 1–2
- **属主**：`catalog/strategy`、`catalog/goal_type`（注册与版本）；`kernel`（在 `step` 内调用并校验返回的意图）；各实现（`strategies/`、`applications/`）
- **头文件**：GoalType 注册在 `include/ayni/ayni.h`（已实现）；策略在 `include/ayni/draft/policy_v1.h`（草案）

策略（规则策略、Tool Loop、未来规划器）与 GoalType 都是有界决策函数。这一形态服务于确定性重放与策略替换，**不是安全边界**：进程内原生代码仍可越权访存或执行 I/O（`authority_and_trust.md`）。

## 调用形态

```
decide(ctx, snapshot, private_checkpoint) -> (intents[≤N], new_private_checkpoint, status)
```

| 输入 | 说明 |
|---|---|
| `snapshot` | 只读任务视图：Goal/Task 与共同检查点、已获准的观察与记忆引用、待决调用结果、相关版本号；回调返回后失效 |
| `private_checkpoint` | 本策略上次返回的私有检查点（ID、版本、内容），只保存算法状态 |

| 输出意图 | 说明 |
|---|---|
| `PROPOSE_GOAL` | 提出目标（受 C-AUTH-004 限制，新目标不产生新权限） |
| `REQUEST_CALL` | 以 `step_key` 请求一次能力调用 |
| `REQUEST_MODEL` | 请求一次模型调用；结果出现在后续快照中 |
| `REPORT_PROGRESS` / `REPORT_COMPLETION` / `REPORT_FAILURE` | 报告步骤进度、完成建议或失败 |
| `WRITE_MEMORY` | 写入记忆（受 C-PROD-031 的版本与 tombstone 检查） |

每个意图携带 `expected_revision` 与关键依赖版本（C-COMMIT-012）。

## GoalType 注册

`ayni_register_goal_type` 在 `CONFIGURING` 中登记一个 GoalType 版本：`goal_type_id`（目录 ID，1–32 字节 `[a-z0-9._-]`）、非零 `version`、调用方持有的 `ctx` 与可选的参数校验函数 `validate`。同一 ID 可以同时登记多个版本，创建 Goal 时指定其中一个。没有 `validate` 的类型只接受空参数；有 `validate` 时，Kernel 先检查参数不超过 `goal_parameter_max_bytes`，再调用 `validate(ctx, parameters)`，返回 `AYNI_OK` 才接受，并保存参数的副本（ADR-0017）。默认策略、等待上限与 `decide` 字段随策略接入（`MA-004` 第三部分）追加在结构末尾。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-POLICY-001 | 策略回调在 `step` 内调用，GoalType 的参数校验在 `ayni_submit_intent` 内调用，二者都在 Kernel 所有者线程上；回调内不得调用 Kernel API、执行 I/O 或阻塞 | Tool Loop 在回调中同步发 HTTP 请求等待模型 | `MA-009`；评审 |
| C-POLICY-002 | 回调只能返回有界意图集合与新的私有检查点；意图数超过注册时声明的上限时整次返回被拒绝，Task 暂停并报告原因 | 策略返回 1,000 个调用意图，Kernel 逐个处理导致 `step` 超时 | `MA-009`；C-LIMIT-006 |
| C-POLICY-003 | Kernel 校验每个意图的 Schema、授权、预算、资源与版本；策略不能修改内部对象、绕过资源入口、提升权限或自行恢复结果未知的动作 | 策略返回“把 ActionRecord 改为 COMPLETED”的意图并被接受 | E3 步骤 2；E4 步骤 1；C-TRUST-001 |
| C-POLICY-004 | 快照只读，且在回调返回后失效；策略不得保存快照中的指针供下次使用 | 策略缓存上次快照中的记忆指针，下次读到已删除的记录 | ASan 用例（M1）；评审 |
| C-POLICY-005 | 模型调用以 `REQUEST_MODEL` 意图表达；模型结果只在后续快照中出现，不在同一次回调中同步获得 | 策略在回调内等待模型结果再决定下一步 | `MA-009` |
| C-POLICY-006 | 相同快照与私有检查点产生相同输出；回调不依赖隐藏的全局状态、真实时间或未经快照提供的随机数 | 规则策略读取系统时间决定是否触发，重放轨迹不一致 | E3 步骤 1–5（确定性轨迹） |
| C-POLICY-007 | 私有检查点带策略 ID 与版本；版本不兼容或策略未知时 Task 暂停并报告原因，不重置 Task（C-LIFE-024） | 新版本策略读不懂旧检查点，于是从头开始规划并重做已完成步骤 | E3 步骤 5 |
| C-POLICY-008 | 策略只通过共同检查点中的 `step_key` 指代步骤；`step_key` 在 Task 内唯一，跨策略替换保持稳定 | 两个策略对同一步骤使用不同名字，换策略后该步骤被当成新步骤 | E3 步骤 4；E4 步骤 2；C-ID-003 |
| C-POLICY-009 | GoalType 声明目标语义、触发、完成条件、暂停点与等待上限；Kernel 只执行通用生命周期与约束；未知的 GoalType 拒绝 | 提交未注册类型的 Goal，Kernel 按默认规则执行 | M1 已实现（未知的类型或版本拒绝，参数由 GoalType 校验）：`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`、`GoalTypeRegistry.tells_an_unknown_id_from_an_unknown_version`、`GoalTypeRegistry.validator_decides_the_parameters`、`GoalTypeRegistry.type_without_validator_takes_no_parameters`；触发、完成条件、暂停点与等待上限随 `MA-004` 第二、三部分；E4 步骤 5 |
| C-POLICY-010 | v0.1 只在停机与重启时替换策略，不支持在线热替换 | 运行中替换策略导致同一 Task 同时被两个策略推进 | E3 步骤 2、4 |
| C-POLICY-011 | 策略与 GoalType 的复杂计算放入异步 Provider；回调的工作量上限在 M1 测量并写入注册描述 | 规划算法在回调中运行 2 秒，阻塞停止事件处理 | `MA-009`；C-LIMIT-011 |
