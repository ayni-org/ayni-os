# 生命周期与状态转换

- **关联**：计划 §2.4、§3.2「生命周期」、§3.6、§4.3、§4.4；ADR-0006、ADR-0007、ADR-0016、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0021、ADR-0022；`MA-004`、`MA-005`；E2 F13，E3 步骤 5
- **原则**：生命周期、持久化状态与等待原因是三个维度。生命周期写在枚举里；“是否已持久接受”由提交序号表达（C-COMMIT-004）；等待原因由待决调用派生（ADR-0007）。表中未列出的转换都是非法转换，返回 `AYNI_INVALID_STATE` 并保持原状态。

## Kernel

| 状态 | 值 | 含义 |
|---|---:|---|
| `CONFIGURING` | 1 | 已创建，可以注册目录；不处理意图 |
| `RECOVERING` | 2 | `ayni_start` 已冻结目录并通过交叉引用校验，`step` 正在异步重放持久前缀并持久化新的 `boot_epoch` |
| `RUNNING` | 3 | 开放准入 |
| `RESTRICTED` | 4 | 恢复发现不兼容、损坏或存储不可写；只读诊断，本地停止路径照常可用；原因经 `ayni_get_kernel_info` 读取 |
| `DRAINING` | 5 | `ayni_begin_shutdown` 之后，收敛在途工作，不再接受新意图 |
| `STOPPED` | 6 | 停机完成，可以 `ayni_destroy` |

| 从 | 到 | 触发 |
|---|---|---|
| — | `CONFIGURING` | `ayni_create` 成功 |
| `CONFIGURING` | `RECOVERING` | `ayni_start` 校验通过 |
| `CONFIGURING` | `CONFIGURING` | `ayni_start` 校验失败（返回错误与 `ayni_error_v1`，可修正后重试）；Host 没有存储端口（`AYNI_UNSUPPORTED`） |
| `RECOVERING` | `RUNNING` | 持久前缀重放完成、新的 `boot_epoch` 已持久化（C-ID-009）、幂等映射重建、授权与资源检查完成 |
| `RECOVERING` | `RESTRICTED` | 版本不支持、已提交区域损坏、关键记录无法验证，或存储拒绝请求、读写失败或结果未知、`boot_epoch` 耗尽（ADR-0016） |
| `RUNNING` | `RESTRICTED` | 受限栅栏中无法确认旧写入方已排空或隔离，或存储仍不可写（C-COMMIT-008） |
| `RUNNING` | `RECOVERING` | 受限栅栏确认旧写入方已隔离，以新 `boot_epoch` 重载 |
| `RUNNING`、`RECOVERING`、`RESTRICTED` | `DRAINING` | `ayni_begin_shutdown` |
| `DRAINING` | `STOPPED` | 在途工作已终结或记录为未知，资源已释放或隔离，全部预留完成槽已收到完成（C-HOST-018） |

各状态下公开函数的返回值（已实现的函数在前，其余为计划；参数错误先于状态检查）：

| 函数 | CONFIGURING | RECOVERING | RUNNING | RESTRICTED | DRAINING | STOPPED |
|---|---|---|---|---|---|---|
| `ayni_get_lifecycle` | OK | OK | OK | OK | OK | OK |
| `ayni_get_kernel_info` | OK | OK | OK | OK | OK | OK |
| `ayni_destroy` | OK | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | OK |
| `ayni_start` | OK；无存储端口时 UNSUPPORTED；GoalType 指定的策略未注册时 NOT_FOUND；其余交叉引用的校验错误（计划） | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| `ayni_step` | INVALID_STATE | OK | OK | OK | OK | INVALID_STATE |
| `ayni_begin_shutdown` | INVALID_STATE | OK | OK | OK | OK（幂等） | OK（幂等） |
| `ayni_register_principal`、`ayni_register_agent`、`ayni_register_goal_type`、`ayni_register_strategy` | OK、参数错误或 CAPACITY_EXCEEDED | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| `ayni_open_session` | NOT_READY | NOT_READY | OK、NOT_FOUND 或 CAPACITY_EXCEEDED | NOT_READY | NOT_READY | NOT_READY |
| `ayni_close_session` | INVALID_STATE | OK 或 NOT_FOUND | OK 或 NOT_FOUND | OK 或 NOT_FOUND | OK 或 NOT_FOUND | INVALID_STATE |
| `ayni_submit_intent` | NOT_READY | NOT_READY | 按意图校验 | NOT_READY | NOT_READY | NOT_READY |
| `ayni_get_agent`、`ayni_get_goal`、`ayni_get_task` | INVALID_STATE | OK 或 NOT_FOUND（标注 `durable_seq`） | OK 或 NOT_FOUND | OK 或 NOT_FOUND（只读诊断） | OK 或 NOT_FOUND | INVALID_STATE |
| 其余 `ayni_register_*` | OK 或参数错误 | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| 其余查询 | INVALID_STATE | OK（标注 `durable_seq`） | OK | OK（只读诊断） | OK | INVALID_STATE |
| `ayni_post_event` | INVALID_STATE | OK（保留到进入 `RUNNING`） | OK（应用） | OK（本地输入丢弃并计数；对账与诊断事件随 `MA-018`、`MA-010`） | OK（丢弃并计数） | INVALID_STATE |

`ayni_submit_intent` 的参数错误与 `UNSUPPORTED`（未实现的意图种类、非零 `command_id`）先于状态检查，所以在任何状态下都先返回（`ayni.h`）；`ayni_post_event` 的参数错误、未实现的事件种类与无效来源同样先于状态检查（ADR-0021）。会话不持久化，重启后为空；`RECOVERING` 与 `RESTRICTED` 中尚无会话，`ayni_close_session` 返回 NOT_FOUND。`ayni_step` 只在 `RUNNING` 中做到期工作并请策略决定 `READY` 的 Task（ADR-0022）。GoalType 的校验函数与策略的 `decide` 都是 Kernel 调用的回调，不得调用 Kernel（C-POLICY-001）。作为兜底，回调内调用接受非 const Kernel 的所有者线程函数（`ayni_destroy`、`ayni_start`、`ayni_step`、`ayni_begin_shutdown`、注册、会话与 `ayni_submit_intent`）时，它们在参数检查之后、按上表检查状态之前返回 INVALID_STATE，不产生效果。

## AgentInstance

`REGISTERED → STARTING → RUNNING ↔ PAUSED → STOPPING → STOPPED`；初始化或检查点不兼容时进入 `FAULTED`，或带原因进入 `PAUSED`。结束一次任务不结束实例。

| 状态 | 值 | 含义 |
|---|---:|---|
| `REGISTERED` | 1 | 已注册，尚无驻留实例（`instance_id` 为 0） |
| `STARTING` | 2 | 策略初始化中 |
| `RUNNING` | 3 | 可以拥有并推进工作 |
| `PAUSED` | 4 | 带原因暂停，不新增工作 |
| `STOPPING` | 5 | 收敛全部所属工作 |
| `STOPPED` | 6 | 已停止，资源已回收 |
| `FAULTED` | 7 | 初始化失败或检查点不兼容 |

M1 已实现的转换：

| 从 | 到 | 触发 |
|---|---|---|
| `REGISTERED` | `RUNNING` | Kernel 进入 `RUNNING`，一个 transition 中启动全部实例（C-LIFE-025）；版本为 1 |
| `RUNNING` | `PAUSED` | `INSTANCE_PAUSE`；名下 Task 停下 |
| `PAUSED` | `RUNNING` | `INSTANCE_RESUME`；名下 Task 按放行条件继续 |
| `RUNNING`、`PAUSED` | `STOPPED` | `INSTANCE_STOP`；同一 transition 中取消名下全部未终结的 Goal 与 Task |

每次转换使该实例的 `revision` 加 1，规则同 Goal（C-COMMIT-012）。策略尚无需初始化，所以暂不经过 `STARTING`；没有在途调用可收敛，所以停止暂不经过 `STOPPING`（ADR-0018）。`PAUSED` 的实例仍可接收新 Goal，其 Task 在恢复后派生；`STOPPED` 或 `FAULTED` 的实例不能成为新 Goal 的 owner（`OWNER_STATE`）。`STARTING`、`FAULTED` 与暂停原因随策略初始化与重启后的检查点兼容（`MA-010`，ADR-0022 决策 1）。

## Goal

`PROPOSED → ACTIVE ↔ PAUSED → SATISFIED / CANCELLED / EXPIRED / FAILED`。需要确认的 Goal 停留在 `PROPOSED`，不生成可执行工作；持续目标在 `ACTIVE` 期间可以多次产生 Task。

| 状态 | 值 | 含义 |
|---|---:|---|
| `PROPOSED` | 1 | 待确认，不生成可执行工作，也不能有子目标 |
| `ACTIVE` | 2 | 可以产生 Task |
| `PAUSED` | 3 | 保留目标，不产生新 Task |
| `SATISFIED` | 4 | 终态：完成条件已满足 |
| `CANCELLED` | 5 | 终态：已取消，`cancel_revision` 记录取消它的版本 |
| `EXPIRED` | 6 | 终态：已失效 |
| `FAILED` | 7 | 终态：已失败 |

| 从 | 到 | 触发 |
|---|---|---|
| — | `ACTIVE` | `GOAL_CREATE` 通过校验；版本为 1 |
| `ACTIVE` | `PAUSED` | `GOAL_PAUSE`；不改变子目标 |
| `PAUSED` | `ACTIVE` | `GOAL_RESUME`；不改变子目标 |
| `PROPOSED`、`ACTIVE`、`PAUSED` | `CANCELLED` | `GOAL_CANCEL` 作用于它或它的祖先，或 `INSTANCE_STOP` 作用于它的 owner；同一 transition 中取消其下未结束的 Task |
| `ACTIVE`、`PAUSED` | `CANCELLED` | `TASK_CANCEL` 取消它的一个 Task，而它此后没有其他未结束的 Task，也不会再有 occurrence；同一 transition 中取消未终结的后代与它们的 Task（ADR-0023） |
| `PROPOSED`、`ACTIVE`、`PAUSED` | `EXPIRED` | `ayni_step` 在它或它的祖先的失效时刻执行失效，暂停与否都一样；同一 transition 中其下未结束的 Task 一并失效（ADR-0020） |
| `ACTIVE` | `SATISFIED`、`FAILED` | 策略的决定使它的一个 Task 完成或失败，而它此后没有其他未结束的 Task，也不会再有 occurrence；同一 transition 中取消未终结的后代与它们的 Task（ADR-0022） |
| `ACTIVE` | `FAILED` | `ayni_step` 跳过它等待超限的 occurrence 后，它没有未结束的 Task，也不会再有 occurrence；`skip_reason` 保留跳过的原因，同一 transition 中取消未终结的后代与它们的 Task；会使某个版本越过最大值时只跳过，Goal 仍是 `ACTIVE`（ADR-0023） |

每次转换使该 Goal 的 `revision` 加 1；改变已是 `2^32−1` 的版本时拒绝整个意图（C-COMMIT-012）。`ayni_step` 把 Goal 已到期的 occurrence 派生为 Task 时，Goal 的生命周期与 `revision` 都不变，只更新触发游标（已派生的最新 occurrence 编号与下一次到期时间）、最近一个 Task 与 `commit_seq`，调用方持有的版本仍可用于暂停或取消（ADR-0018、ADR-0019）；等待超限而跳过 occurrence 时同样只更新游标、跳过计数与原因和 `commit_seq`（ADR-0020），除非跳过使 Goal 进入 `FAILED`（ADR-0023）。终态 Goal 不再有下一次 occurrence，也不再有失效时刻。失效是 Kernel 自身的转换：会使某个版本越过最大值时不执行，放弃该 Goal 的失效（ADR-0020）。M1 不生成 `PROPOSED`：确认流程随授权与审批（`MA-006`）。Goal 随它的最后一个 Task 或最后一个 occurrence 结束：Task 被决定完成、决定失败或被 `TASK_CANCEL` 取消时分别进入 `SATISFIED`、`FAILED`、`CANCELLED`，occurrence 被跳过时进入 `FAILED`（ADR-0022、ADR-0023）。终态 Goal 在 Goal 表满时让位给新 Goal，最早结束的先让，同时结束的先让深度大的；它已结束的 Task 一并移除，此后查询返回 `NOT_FOUND`（ADR-0025）。

## Task

`READY ↔ RUNNING → COMPLETED`；也可以进入 `PAUSED`、`CANCELLED`、`FAILED`、`EXPIRED`；异步取消尚未完成时为 `CANCELLING`。等待模型、资源、审批或动作是 `blocked_on` 视图，可以同时存在多项，不写入生命周期。

| 状态 | 值 | 含义 |
|---|---:|---|
| `READY` | 1 | 可以前进，等待策略的决定 |
| `RUNNING` | 2 | 策略已发出请求，等待其结果 |
| `PAUSED` | 3 | 已停下，放行条件恢复后继续 |
| `CANCELLING` | 4 | 已请求取消，在途请求尚未收敛 |
| `COMPLETED` | 5 | 终态：已完成 |
| `CANCELLED` | 6 | 终态：已取消，`cancel_revision` 记录取消它的版本 |
| `FAILED` | 7 | 终态：已失败 |
| `EXPIRED` | 8 | 终态：已失效 |

M1 已实现的转换（ADR-0018、ADR-0022、ADR-0024）：

| 从 | 到 | 触发 |
|---|---|---|
| — | `READY` | `ayni_step` 派生 Goal 的 occurrence，截至当时已到期的全部 occurrence 合并为一个 Task（ADR-0019）；版本为 1 |
| `READY` | `PAUSED` | Goal 或实例的改变使它不再满足放行条件 |
| `PAUSED` | `READY` | Goal 或实例的改变使它重新满足放行条件；策略的决定使其停下的 Task 除外 |
| `READY` | `COMPLETED`、`FAILED` | `ayni_step` 中策略的决定结束它；决定中报告进度的意图同时提交其步骤，私有检查点替换 Task 的 |
| `READY` | `READY` | `ayni_step` 中策略的决定报告进度而不结束它（自 0.12）：提交步骤并替换私有检查点，版本加 1，`commit_seq` 取该 transition 的值，在之后的 step 中再被决定（ADR-0024） |
| `READY` | `PAUSED` | `ayni_step` 中策略的决定被拒绝，`hold_status`、`hold_reason` 记下原因；之后不再被决定，也不再放行，结束前它的 Goal 不派生新 Task（`TASK_HELD`） |
| `READY`、`PAUSED` | `CANCELLED` | `TASK_CANCEL`；`GOAL_CANCEL` 作用于它的 Goal 或其祖先；`INSTANCE_STOP` 作用于它的 owner；同一 transition |
| `READY`、`PAUSED` | `EXPIRED` | 它的 Goal 或其祖先失效；同一 transition（ADR-0020） |

放行条件：Task 只在它的 Goal 与该 Goal 的全部祖先都是 `ACTIVE`、owner 实例是 `RUNNING` 时前进。暂停不级联：暂停的 Goal 或实例不改写子目标的状态，只让其下的 Task 停下，恢复后自动继续。Goal 或实例的每次改变在同一个 transition 中按放行条件重算它触及的 `READY`/`PAUSED` Task，状态改变的才提升版本；派生也只发生在放行的 Goal 上。每次转换使该 Task 的 `revision` 加 1，规则同 Goal（C-COMMIT-012）。M1 到达 `READY`、`PAUSED`、`COMPLETED`、`CANCELLED`、`FAILED`、`EXPIRED`：决定在同一 step 中应用，`RUNNING` 留给等待在途请求结果的 Task，`CANCELLING` 随可取消的在途请求（`MA-008`，ADR-0022）。结束的 Task 在 Task 表满时让位给新 Task，此后查询返回 `NOT_FOUND`（ADR-0018）。

## ActionRecord

| 从 | 到 | 条件 |
|---|---|---|
| — | `PROPOSED` | 意图通过 Schema 校验 |
| `PROPOSED` | `PREPARED` | 授权与参数通过，准备记录（意图、步骤身份、绑定）持久化 |
| `PROPOSED` | `REJECTED` | 授权、参数或预算拒绝 |
| `PREPARED` | `DISPATCHING` | 再次校验通过，可能派发屏障持久化成功（C-COMMIT-006） |
| `PREPARED` | `CANCELLED` | 取消或条件失效（确定未派发） |
| `DISPATCHING` | `IN_PROGRESS` | Host 报告执行中 |
| `DISPATCHING` | `CANCELLED` | 屏障后复核拒绝且可证明未交给 Host，`effect=NONE` 已提交（C-COMMIT-007） |
| `DISPATCHING`、`IN_PROGRESS` | `COMPLETED` | 完成结果持久化 |
| `IN_PROGRESS` | `FAILED` | 确定失败结果持久化 |
| `DISPATCHING`、`IN_PROGRESS` | `RESULT_UNKNOWN` | 重启或无法确认结果 |
| `RESULT_UNKNOWN` | `COMPLETED` / `FAILED` | 权威查询确认（C-CALL-010） |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-LIFE-001 | `ayni_create` 成功后 Kernel 处于 `CONFIGURING` | 创建后直接处于 `RUNNING` 并开始接受意图 | M0 已实现：`KernelApi.create_in_caller_memory_starts_configuring` |
| C-LIFE-002 | `ayni_destroy` 只在 `CONFIGURING` 或 `STOPPED` 成功；其他状态返回 `AYNI_INVALID_STATE` 且 Kernel 不变；`STOPPED` 意味着没有 Host 或 Provider 仍持有完成槽（C-HOST-018） | 运行中的 Kernel 被销毁，在途 I/O 回调写入已释放的内存 | M1 已实现：`KernelApi.kernel_memory_is_reusable_after_destroy`、`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`；`MA-005` |
| C-LIFE-003 | 注册只在 `CONFIGURING` 进行；`ayni_start` 一次性校验全部交叉引用，注册顺序无关；引用缺失时 start 失败并保持 `CONFIGURING` | 先注册 Binding 后注册 Capability 导致失败 | M1 已实现（注册只在 `CONFIGURING`；`ayni_start` 解析 GoalType 指定的策略，缺少时返回 `NOT_FOUND` 并保持 `CONFIGURING`，补登记后可再启动，ADR-0022）：`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.return_values_follow_the_state_table`、`WorkStrategy.start_resolves_strategy_ids_and_waits_for_a_missing_one`、`GoalTypeRegistry.link_strategies_fails_on_a_missing_strategy_and_may_run_again`；其余交叉引用随引用其他目录项的注册（绑定：`MA-008`） |
| C-LIFE-004 | 非 `RUNNING` 时 `ayni_submit_intent` 返回 `AYNI_NOT_READY`，不排队、不改变状态 | 恢复期间到达的创建提醒命令被排队，恢复后又按重建的幂等映射再次创建 | M1 已实现：`WorkModel.intent_outside_running_is_not_ready`、`WorkModel.draining_answers_queries_and_closes_sessions_but_takes_no_work`、`WorkModel.return_values_follow_the_state_table`；E2 F13 |
| C-LIFE-005 | 生命周期变化经事件与 `ayni_get_lifecycle` 同时可见；事件在状态已应用之后发出 | 事件报告 `RUNNING` 时查询仍返回 `RECOVERING` | `MA-005` |
| C-LIFE-006 | 首版不提供运行时注销；运行时发现的目录变化另行定义为经提交协调的 transition | 运行中删除 Capability，在途 ActionRecord 失去语义 | `MA-005` |
| C-LIFE-007 | 已实现的函数在每个状态下按上表返回；表中的 `INVALID_STATE` 不改变状态；进入 `RESTRICTED` 时记录原因，`ayni_get_kernel_info` 只在 `RESTRICTED` 中报告它（ADR-0016） | 恢复失败后状态停在 `RECOVERING`，调用方无法区分“仍在恢复”与“已经失败” | M1 已实现：`KernelLifecycle.return_values_follow_the_state_table`、`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.restriction_is_reported_only_while_restricted`；`MA-005` |
| C-LIFE-010 | ActionRecord 只按上表转换；`RESULT_UNKNOWN` 只能经权威查询离开 | `RESULT_UNKNOWN` 超时后自动改为 `FAILED` 并重试 | E2 F3–F5；状态转换表测试 |
| C-LIFE-011 | 运行终态与副作用结果分开：`CANCELLED`/`FAILED` 的记录列出已完成、未执行与未知的步骤，不推导“没有影响” | 取消后报告“无影响”，但第一步转向已执行 | E4 步骤 3 |
| C-LIFE-012 | 取消请求已受理、停止指令已送达、物理输出已停止是三种事实，分别表达 | 把“已发送停止”显示为“已停止” | E1 步骤 4；C-STOP-006 |
| C-LIFE-013 | 撤销响应区分 `ACCEPTED`、`DURABLY_EFFECTIVE`、`CANCELLING`、`STOP_CONFIRMED`；只有最后一项证明本地物理停止 | 授权版本生效即报告设备已停止 | E1 步骤 6；E2 F10 |
| C-LIFE-020 | 已接受的 Goal/Task 固定 owner 实例与设备；失联不自动转交其他实例 | 设备 A 离线后，设备 B 上的同名实例接管 A 的提醒 | M1 已实现（Goal：创建时指定本 Kernel 的实例，之后没有改变 owner 的意图；Task 继承其 Goal 的 owner，ADR-0018）：`WorkModel.owner_must_be_an_instance_of_this_kernel`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.child_must_have_its_parents_owner`、`WorkTask.once_goal_derives_one_task_in_the_next_step`、`WorkTask.instance_pause_holds_its_tasks_and_resume_releases_them`；`MA-016` |
| C-LIFE-021 | 暂停 Goal 保留目标与检查点，停止产生新 Task；在途 Task 按该类型声明的暂停点收敛；无法立即暂停时显示“暂停中” | 暂停目标后仍派生新 Task | M1 已实现（Goal 状态与版本；暂停的 Goal 及其后代不派生 Task，已有的 Task 停下，ADR-0018）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.pause_and_resume_change_only_the_goal`、`GoalTable.lifecycle_decides_which_changes_are_allowed`、`WorkTask.paused_ancestor_holds_the_tasks_below_it_until_resumed`、`WorkTask.goal_created_under_a_paused_parent_derives_once_the_parent_resumes`、`WorkTask.paused_goal_holds_its_own_task`、`WorkTask.task_waits_for_both_its_goal_and_its_owner`；暂停与恢复保留 Task 的步骤与私有检查点，恢复后的决定从它们开始（ADR-0024）：`WorkCheckpoint.pause_keeps_the_steps_and_checkpoint_for_the_next_decision`；在途 Task 的暂停点随 `MA-008`、`MA-018`（ADR-0022） |
| C-LIFE-022 | 取消 Goal/Task 标记取消版本，禁止新增派生步骤，并传播到所属在途请求；已完成影响保留，补偿是另一次需授权的操作 | 取消后自动反向转动以“回滚”物理世界 | M1 已实现（Goal 及其后代与它们的 Task 在一个 transition 中取消并记录 `cancel_revision`；取消的 Goal 不再派生 Task；取消 Goal 的最后一个 Task 而它不会再有 occurrence 时 Goal 随之取消，ADR-0023）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.cancel_reaches_every_live_descendant_and_nothing_else`、`WorkTask.cancel_reaches_the_tasks_of_the_goal_and_its_descendants`、`WorkTask.task_cancel_of_a_one_shot_goals_task_cancels_the_goal`、`WorkTask.task_cancel_cancels_a_one_shot_goal_whatever_holds_its_task`、`WorkTask.task_cancel_ends_a_goal_only_with_its_last_task`、`WorkTask.task_cancel_ends_an_interval_goal_whose_next_time_never_comes`、`WorkInput.task_cancel_leaves_the_goal_taking_inputs`、`TaskTable.pause_resume_and_cancel_follow_the_task_lifecycle`，子目标排在父目标之前时同样（ADR-0025）`WorkTask.child_in_an_earlier_place_follows_its_parent`；传播到在途请求随 `MA-008`、`MA-018`（ADR-0022）；E4 步骤 3 |
| C-LIFE-023 | 实例暂停阻止其新增工作并到兼容检查点；停止收敛全部所属工作并回收资源；“暂停助理”不映射为暂停全部实例（C-PROD-020） | 暂停助理时把提醒所属实例一并暂停 | M1 已实现（实例暂停使名下 Task 停下、恢复后继续，暂停中的实例仍接收新 Goal 而不派生；停止在一个 transition 中取消名下全部未终结的 Goal 与 Task，ADR-0018）：`WorkTask.instance_pause_holds_its_tasks_and_resume_releases_them`、`WorkTask.paused_instance_takes_goals_and_derives_after_resume`、`WorkTask.held_goal_keeps_no_later_goal_from_deriving`、`WorkTask.task_waits_for_both_its_goal_and_its_owner`、`WorkTask.instance_stop_cancels_its_goals_and_tasks`、`AgentTable.pause_resume_and_stop_follow_the_instance_lifecycle`；实例暂停保留 Task 的步骤与私有检查点（ADR-0024）：`WorkCheckpoint.pause_keeps_the_steps_and_checkpoint_for_the_next_decision`；重启后的兼容检查点随 `MA-010`，资源回收随 `MA-018`；“暂停助理”的集合随 `MA-019`；E1 步骤 11 |
| C-LIFE-024 | 不兼容的私有检查点或未知策略版本使 Task 暂停并报告原因，保留原记录，不重置 Task 或改写完成条件 | 换策略失败后 Task 被重置为 `READY` 重新开始 | E3 步骤 5 |
| C-LIFE-025 | Kernel 进入 `RUNNING` 时，在一个 transition 中为每个已注册的 Agent 创建驻留实例并分配 `instance_id`（C-ID-010）；没有 Agent 时不产生 transition。每个 Agent 在一个 Kernel 上只有一个实例，Goal 只能以本 Kernel 的实例为 owner | 两个实例在不同 transition 中启动，查询看到一个已运行、一个仍未分配 ID 的中间状态 | M1 已实现（实例尚未持久化，每次启动重新创建）：`WorkModel.instances_start_with_the_kernel_in_one_transition`、`AgentTable.start_all_gives_each_instance_an_id_and_the_stamp`、`AgentTable.registers_each_agent_once_up_to_capacity`；跨重启保持实例随 `MA-010`；策略初始化与 `STARTING` 随 `MA-010`（ADR-0022） |
