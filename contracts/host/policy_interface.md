# 策略与 GoalType 接口

- **关联**：计划 §2.5、§2.7、§3.3「新策略」、§5.3；ADR-0006、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0021、ADR-0022、ADR-0024；`MA-004`、`MA-009`、`MA-019`；E3 步骤 2–5，E4 步骤 1–2
- **属主**：`catalog/strategy`、`catalog/goal_type`（注册与版本）；`kernel`（在 `step` 内调用并校验返回的意图）；各实现（`strategies/`、`applications/`）
- **头文件**：GoalType 与策略的注册、任务快照、私有检查点、报告进度与结束 Task 的意图在 `include/ayni/ayni.h`（已实现）；其余意图种类在 `include/ayni/draft/policy_v1.h`（草案）

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

M1 已实现的形态（ABI 0.10，ADR-0022）是 `decide(ctx, snapshot, output) -> status`：快照是一个 `READY` Task 的只读视图（Task 的版本、ID、Goal、owner 实例、代表的 occurrence 与到期时间、决定它的 step 开始的时刻、GoalType 的 ID 与版本、Goal 参数），输出的意图数组由 Kernel 提供。自 0.12 起（ADR-0024），快照还带 Task 已提交的步骤与私有检查点（写入它的策略 ID、版本与内容），输出另有 Kernel 提供的私有检查点缓冲，开始时是该检查点的副本；意图为 `REPORT_PROGRESS`（以内联的 `step_key` 提交一个步骤）、`REPORT_COMPLETION` 与 `REPORT_FAILURE`，各带 `expected_revision`。依赖版本、正文与其余种类随调用（`MA-008`）。

## GoalType 注册

`ayni_register_goal_type` 在 `CONFIGURING` 中登记一个 GoalType 版本：`goal_type_id`（目录 ID，1–32 字节 `[a-z0-9._-]`）、非零 `version`、调用方持有的 `ctx` 与可选的参数校验函数 `validate`。同一 ID 可以同时登记多个版本，创建 Goal 时指定其中一个。没有 `validate` 的类型只接受空参数；有 `validate` 时，Kernel 先检查参数不超过 `goal_parameter_max_bytes`，再调用 `validate(ctx, parameters)`，返回 `AYNI_OK` 才接受，并保存参数的副本（ADR-0017）。自 0.10 起，会产生 Task 的 GoalType（`trigger_kinds` 不只是 `NONE`）以 `strategy_id` 指定决定其 Task 的策略，只接受 `NONE` 的 GoalType 不指定；`ayni_start` 解析全部策略 ID（ADR-0022）。Task 执行中的等待上限与暂停点的决定函数随资源等待（`MA-018`）追加在结构末尾。

## 策略注册与决策

`ayni_register_strategy` 在 `CONFIGURING` 中登记一个策略：目录 ID、非零版本、调用方持有的 `ctx`、`decide`、每次调用的意图上限 `max_intents_per_call`（1 到 `policy_intent_capacity`），以及策略写意图的步长 `intent_size`（它编译时的 `sizeof(ayni_policy_intent_v1)`，须等于本库的大小，规则同 `struct_size`）。同一 ID 同时只登记一个版本（C-POLICY-010）。`ayni_step` 在 `RUNNING` 中做完到期工作后，按进入 `READY` 的先后请策略决定本次 step 之前已是 `READY` 的 Task，每个决定是一个 item 与一个 transition（ADR-0022）：

| 决定 | 结果 |
|---|---|
| 一个结束意图，版本与快照一致 | Task 进入 `COMPLETED` 或 `FAILED`；Goal 不再有未结束的 Task、也不会再有 occurrence 时，同一 transition 中进入 `SATISFIED` 或 `FAILED`，其未终结的后代与它们的 Task 取消；决定中的 `REPORT_PROGRESS` 同时提交其步骤，私有检查点替换 Task 的 |
| 一个或多个 `REPORT_PROGRESS`、没有结束意图（自 0.12） | 步骤按顺序提交，私有检查点替换 Task 的；Task 保持 `READY`，版本加 1，在之后的 step 中排在在它之前已是 `READY` 的 Task 之后再被决定。步骤在 Task 内唯一，至多 `task_step_capacity` 个，所以一个 Task 至多被应用 `task_step_capacity` + 1 个决定 |
| 其他任何决定，包括只改私有检查点、没有意图的决定 | Task 停在 `PAUSED`，`hold_status` 与 `hold_reason` 记下第一项未通过的检查，步骤与私有检查点不变；不再被决定，Goal 与实例的恢复也不放行它，取消与失效照常结束它。它结束之前，它的 Goal 不派生新 Task，occurrence 以 `TASK_HELD` 等待，超过等待上限后跳过 |

检查顺序见 `ayni_decide_fn` 的注释：`decide` 的失败状态或不是状态码的返回值，意图数与私有检查点的大小，没有意图，然后逐个意图检查结构大小、种类与版本，`REPORT_PROGRESS` 再检查 `step_key`、重复的步骤（`STEP_TAKEN`）与步骤空位（`STEP_CAPACITY`），结束意图再检查 `step_key_size` 为 0 与重复的结束意图，最后是会使版本越过最大值的结束。策略以 `max_checkpoint_bytes`（0 到 `task_checkpoint_max_bytes`）声明私有检查点的上限；Kernel 只读回 `intent_count`、`checkpoint_size` 与自己的缓冲（ADR-0024）。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-POLICY-001 | 策略回调在 `step` 内调用，GoalType 的参数校验在 `ayni_submit_intent` 内调用，二者都在 Kernel 所有者线程上；回调内不得调用 Kernel API、执行 I/O 或阻塞；作为兜底，回调期间修改 Kernel 的调用返回 `AYNI_INVALID_STATE` 且不产生效果 | Tool Loop 在回调中同步发 HTTP 请求等待模型 | M1 已实现（GoalType 的校验与策略的 `decide` 期间，修改 Kernel 的调用返回 `INVALID_STATE` 且不产生效果，ADR-0022）：`WorkModel.validator_cannot_change_the_kernel_while_it_runs`、`WorkDecision.strategy_cannot_change_the_kernel_while_it_decides`；不执行 I/O、不阻塞只能评审；`MA-009` |
| C-POLICY-002 | 回调只能返回有界意图集合与新的私有检查点；意图数超过注册时声明的上限时整次返回被拒绝，Task 暂停并报告原因 | 策略返回 1,000 个调用意图，Kernel 逐个处理导致 `step` 超时 | M1 已实现（超过 `max_intents_per_call` 的意图、超过 `max_checkpoint_bytes` 的私有检查点或策略返回 `CAPACITY_EXCEEDED` 时整次决定被拒绝，Task 停下并记 `POLICY_OUTPUT_LIMIT`，不提交步骤与检查点；上限在注册时检查，ADR-0022、ADR-0024）：`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.kernel_reads_only_its_own_checkpoint_buffer`、`WorkStrategy.registration_checks_its_arguments`；`MA-009`；C-LIMIT-006 |
| C-POLICY-003 | Kernel 校验每个意图的 Schema、授权、预算、资源与版本；策略不能修改内部对象、绕过资源入口、提升权限或自行恢复结果未知的动作 | 策略返回“把 ActionRecord 改为 COMPLETED”的意图并被接受 | M1 已实现（结构大小、种类、版本与互相矛盾的结束意图；`step_key`、重复的步骤与步骤上限；策略只写 Kernel 提供的意图数组与私有检查点缓冲，注册时确认它写意图的步长与 Kernel 一致，Kernel 只读回自己的缓冲，并按自己保存的上限读私有检查点，ADR-0022、ADR-0024）：`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkStrategy.registration_checks_its_arguments`、`WorkDecision.strategy_starts_from_cleared_intents_that_only_the_kernel_reads`、`WorkCheckpoint.kernel_reads_only_its_own_checkpoint_buffer`、`WorkDecision.end_that_would_exhaust_a_revision_holds_the_task`、`WorkCheckpoint.end_that_would_exhaust_a_revision_commits_nothing`；授权、预算与资源随 `MA-006`、`MA-018`；E3 步骤 2；E4 步骤 1；C-TRUST-001 |
| C-POLICY-004 | 快照只读，且在回调返回后失效；策略不得保存快照中的指针供下次使用 | 策略缓存上次快照中的记忆指针，下次读到已删除的记录 | ASan 用例（M1）；评审 |
| C-POLICY-005 | 模型调用以 `REQUEST_MODEL` 意图表达；模型结果只在后续快照中出现，不在同一次回调中同步获得 | 策略在回调内等待模型结果再决定下一步 | `MA-009` |
| C-POLICY-006 | 相同快照与私有检查点产生相同输出；回调不依赖隐藏的全局状态、真实时间或未经快照提供的随机数 | 规则策略读取系统时间决定是否触发，重放轨迹不一致 | E3 步骤 1–5（确定性轨迹） |
| C-POLICY-007 | 私有检查点带策略 ID 与版本；版本不兼容或策略未知时 Task 暂停并报告原因，不重置 Task（C-LIFE-024） | 新版本策略读不懂旧检查点，于是从头开始规划并重做已完成步骤 | M1 已实现（私有检查点带写入它的策略 ID 与版本，下一次决定从它开始；被拒绝的决定不改变它，ADR-0024）：`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.task_in_a_given_up_place_starts_without_steps_or_checkpoint`；同一次启动中只有一个版本，版本不兼容与未知策略时的暂停随重启（`MA-010`）；E3 步骤 5 |
| C-POLICY-008 | 策略只通过共同检查点中的 `step_key` 指代步骤；`step_key` 在 Task 内唯一，跨策略替换保持稳定 | 两个策略对同一步骤使用不同名字，换策略后该步骤被当成新步骤 | M1 已实现（`REPORT_PROGRESS` 以目录 ID 形式的 `step_key` 提交步骤，在 Task 内唯一，只增不删，快照按提交顺序给出，ADR-0024）：`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.steps_bound_how_often_a_task_is_decided`、`WorkCheckpoint.steps_that_share_a_prefix_are_distinct`、`TaskTable.decisions_commit_steps_checkpoints_and_progress`、`TaskTable.add_step_refuses_a_taken_invalid_or_extra_step`；跨策略替换随重启（`MA-010`），调用的步骤随 `MA-008`；E3 步骤 4；E4 步骤 2；C-ID-003 |
| C-POLICY-009 | GoalType 声明目标语义、触发、完成条件、暂停点与等待上限；Kernel 只执行通用生命周期与约束；未知的 GoalType 拒绝 | 提交未注册类型的 Goal，Kernel 按默认规则执行 | M1 已实现（未知的类型或版本拒绝，参数由 GoalType 校验；GoalType 声明它接受的触发种类，Goal 只能选择其中之一，M1 为 `NONE`、`ONCE`、`DELAY`、`INTERVAL`、`LOCAL_INPUT`，ADR-0018、ADR-0019、ADR-0021；GoalType 声明 occurrence 等待 Task 槽位或 ID 的上限，ADR-0020；会产生 Task 的 GoalType 指定决定其 Task 的策略，完成由策略的决定表达，ADR-0022）：`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`、`GoalTypeRegistry.tells_an_unknown_id_from_an_unknown_version`、`GoalTypeRegistry.validator_decides_the_parameters`、`GoalTypeRegistry.type_without_validator_takes_no_parameters`、`GoalTypeRegistry.trigger_kinds_decide_the_accepted_triggers`、`WorkTask.goal_type_must_accept_the_trigger`、`WorkInput.goal_type_must_accept_local_input`、`WorkModel.registration_checks_its_arguments`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.zero_wait_skips_at_once`、`WorkWait.unbounded_wait_never_skips`、`WorkStrategy.goal_type_names_a_strategy_exactly_when_its_goals_may_have_tasks`、`WorkDecision.goal_ends_with_the_last_of_its_tasks`；观察与订阅事件的触发随 `MA-018`，日历规则随 `MA-011`，暂停点与 Task 执行中的等待上限随 `MA-008`、`MA-018`；E4 步骤 5 |
| C-POLICY-010 | v0.1 只在停机与重启时替换策略，不支持在线热替换 | 运行中替换策略导致同一 Task 同时被两个策略推进 | M1 已实现（策略只在 `CONFIGURING` 中注册，同一 ID 同时只有一个版本，ADR-0022）：`WorkStrategy.registration_checks_its_arguments`、`StrategyRegistry.refuses_another_version_of_an_id_and_registrations_beyond_capacity`、`WorkModel.return_values_follow_the_state_table`；重启后换版本的检查点兼容随 `MA-010`；E3 步骤 2、4 |
| C-POLICY-011 | 策略与 GoalType 的复杂计算放入异步 Provider；回调的工作量上限在 M1 测量并写入注册描述 | 规划算法在回调中运行 2 秒，阻塞停止事件处理 | `MA-009`；C-LIMIT-011 |
