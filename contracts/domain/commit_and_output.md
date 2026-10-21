# 提交与输出门控

- **关联**：计划 §2.4、§2.8、§4.3；ADR-0002、ADR-0017、ADR-0018、ADR-0020、ADR-0022、ADR-0026；`MA-004`、`MA-010`、`MA-022`；E2 F0–F13
- **属主**：`durability`（持久前缀、重放、栅栏记录）；`kernel`（transition 编排、输出门控、受限栅栏、恢复编排）

## 术语

| 术语 | 含义 |
|---|---|
| transition | Kernel 在所有者线程上的一次状态推进，按 `commit_seq` 顺序立即应用到唯一一份内存状态 |
| 提交批次 | 一个 transition 的全部持久记录，带 `transition_id`、`commit_seq`、长度、校验与提交标记 |
| `durable_seq` | 连续已持久的最长前缀的最后一个 `commit_seq` |
| `requires_seq` | 一个对外输出所依赖的最大 `commit_seq` |
| 对外输出 | 持久接受回复、对外事件、通知、有副作用的 Host submit、执行许可签发与续租 |
| 受限栅栏（FENCED） | 写入失败或结果未知、重试仍失败后的状态：停止新输出、新准入与许可续租 |

## 最小提交批次

以下每行的记录必须在同一批次中，全部可见或全部不可见（计划 §2.8）：

| 批次 | 内容 | 恢复不变量 |
|---|---|---|
| B1 | 命令幂等映射 + Goal/Task 接受结果 | 响应丢失后重试返回相同对象，不新增第二个提醒 |
| B2 | 触发 occurrence/游标 + Task 创建 + Budget 预留 | 不漏触发，也不生成新 Task 绕过去重 |
| B3 | 步骤检查点 + `logical_call_id`/`operation_id` + 参数摘要与绑定 | 恢复沿用原调用身份；策略替换不把已做步骤当成新动作 |
| B4 | 调用结果应用 + 任务进度/记忆变更 + Budget 结算 | 不重复写摘要，不双重结算；提交失败仍可对账 |
| B5 | 删除/撤销版本 + 依赖失效索引与处理游标 | 源记录立即对新使用不可见；派生清理由可恢复的有界工作完成 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-COMMIT-001 | 持久层的 `append(batch)` 以 `OK`、`FAILED`、`UNKNOWN` 三者之一完成；对外只暴露 `durable_seq` | 存储 Host 在数据进入写缓冲时就报告 OK | M1 已实现（确认持久、同身份重试）：`WorkJournal.result_is_durable_once_the_host_stores_its_batch`、`StorageRecovery.failed_or_unknown_append_retries_without_duplicate_application`、`HostStorage.repeated_batch_identity_keeps_one_copy_and_refuses_changed_bytes`；Native 在同步成功后才确认；C-HOST-010 |
| C-COMMIT-002 | 恢复精确返回连续持久前缀并忽略不完整的尾部；同一 transition 重放不重复应用 | 短写的尾部批次被部分应用 | M1 已实现（工作模型：恢复按序重放每个已存储批次各一次，追加中断电的批次只在完整写入时计入，ADR-0026）：`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`、`HostStorage.torn_tail_is_dropped_and_overwritten`；E2 F7、F8 |
| C-COMMIT-003 | Kernel 只维护一份内存状态，transition 按 `commit_seq` 立即应用；不建暂存副本，不在内存中回滚部分 transition | 写失败后把内存中的预算扣减“退回”，但后续准入已依赖它 | `MA-010` 属性测试（ADR-0002 验证）；transition 开始前先确认环中有最大批次的空间，没有空间时不应用，写入失败时不回滚而是受限（ADR-0026）：`WorkJournal.transition_waits_for_room_for_its_batch`、`WorkJournal.append_that_fails_restricts_the_kernel`；Goal 意图在校验全部通过后才取得 ID 与 transition 并应用，拒绝不改变状态：`WorkModel.full_goal_table_refuses_without_side_effects`、`WorkModel.stale_revision_is_a_conflict_and_changes_nothing`、`WorkModel.change_past_the_largest_revision_is_refused_and_changes_nothing` |
| C-COMMIT-004 | 每个对外输出带 `requires_seq`，只在 `durable_seq ≥ requires_seq` 后释放；查询可以返回已应用的状态，但必须标注 `durable_seq`，不得把未覆盖部分报告为已持久 | 创建提醒的 HTTP 回复在日志写入前发出，掉电后提醒消失 | M1 已实现（查询标注：对象带 `commit_seq`，`ayni_get_kernel_info` 报告 `durable_seq`，结果未知的追加不计入）：`KernelLifecycle.durable_seq_follows_the_log_read_and_then_the_boot_batch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`WorkModel.created_goal_is_applied_at_once_and_not_yet_durable`；`durable_seq` 推进到 Host 已存储的批次，步骤报告也给出它（ADR-0026）：`WorkJournal.result_is_durable_once_the_host_stores_its_batch`、`KernelLifecycle.step_reports_durable_seq_when_the_report_covers_it`；对外输出的 `requires_seq` 门控随产生对外输出的包；E2 F6、F9；属性测试：每个已释放输出的 seq ≤ 恢复后的 `durable_seq` |
| C-COMMIT-005 | 上表 B1–B5 的每组记录在同一提交批次中 | Task 已创建但 occurrence 游标未提交，重启后再次触发 | M1 部分实现（每个 transition 一个批次：B1 的命令映射与 Goal、B2 的 Task 与 occurrence 游标、B3 的步骤与私有检查点各在同一批次中，ADR-0026）：`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`WorkCommands.mappings_survive_a_restart_with_their_transitions`；调用身份、Budget、记忆与撤销的记录随各自的包；E2 F8、F11 |
| C-COMMIT-006 | 必须先持久化 `DISPATCHING` 屏障（`durable_seq` 覆盖屏障），再允许 Host 发出有副作用的请求；屏障写失败则禁止派发 | 屏障与 submit 并行发出，屏障写失败但动作已执行 | E2 F2 |
| C-COMMIT-007 | 屏障确认后，在所有者线程再次检查 Grant/Task/Scene/观察、依赖 tombstone 与停止栅栏；复核与 submit 之间不处理其他普通意图。复核失败且可证明未交给 Host 时持久记录 `CANCELLED` 与 `effect=NONE`；该记录提交前重启仍保守为 `RESULT_UNKNOWN` | 复核通过后先处理了一条撤销意图，再 submit 已被撤销的动作 | E2 F10 |
| C-COMMIT-008 | 写入 `FAILED` 或 `UNKNOWN` 时，以相同 `transition_id`/`commit_seq` 有界重试；重试成功按幂等重放处理；仍失败则进入受限栅栏 | `UNKNOWN` 后以新 `commit_seq` 重写同一 transition，恢复时应用两次 | M1 已实现（三次尝试、10/50 ms 退避、失败后暂停、same command 先于 BUSY）：`KernelFence.persisted_unknown_retries_one_batch_and_freezes_new_commands`、`KernelFence.normal_append_in_flight_keeps_room_for_later_transitions`、`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`StorageRecovery.retry_wait_is_readable_and_preserves_command_lookup_before_busy`；E2 在途动作随 `MA-022` |
| C-COMMIT-009 | 受限栅栏中：立即停止新输出、新准入与执行许可续租；已越过屏障的在途动作按能力属性继续或取消；其完成事件只进入有界易失缓冲供对账，不直接改写状态 | 栅栏期间继续续租执行许可，电机凭新租期一直转 | M1 部分实现（无 Provider/许可时停止新工作、事件不跨重载、查询诊断）：`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`KernelFence.unavailable_isolation_preserves_diagnostics_without_new_boot`；许可与动作对账随 `MA-018`、`MA-022`；E2 F12 |
| C-COMMIT-010 | 重载前，持久层须确认旧写入方已失去修改日志的能力：存储 Host 报告旧 append/flush 全部终止（排空），或原子地安装新的 writer epoch 使旧 epoch 写入被拒绝；日志随之写入栅栏记录，恢复时忽略栅栏记录之后出现的旧 epoch 批次 | 只拒绝旧代际的完成事件，旧 flush 在恢复读取之后落盘，状态与日志分叉 | M1 已实现（Native 排空、Sim 持久 epoch、WRITER_FENCE、物理 cursor）：`HostStorage.writer_fence_isolates_append_snapshot_and_trim`、`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`SimStorageFaults.fence_rejects_an_owned_late_write_after_its_unknown_completion`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`；E2 F12 |
| C-COMMIT-011 | 两种确认都无法取得时保持只读诊断（`RESTRICTED`），不重载、不恢复输出；存储仍不可写时同样进入只读诊断，本地停止路径照常可用 | 无法隔离旧写入方时仍然重载并恢复输出 | M1 已实现（隔离不可确认则只读、不生成新 boot）：`KernelFence.unavailable_isolation_preserves_diagnostics_without_new_boot`；含动作与执行许可的停止验收随 `MA-022`；E2 F12 |
| C-COMMIT-012 | 所有策略意图携带 `expected_revision` 与关键依赖版本；过时写入返回 `AYNI_REVISION_CONFLICT`，由上层重新评估，不静默覆盖。`revision` 不回绕：意图要改变的对象中有版本已是 2^32−1 的，整个意图返回 `AYNI_CAPACITY_EXCEEDED`（`REVISION_EXHAUSTED`），不改变任何对象 | 旧模型结果覆盖了用户刚修改的记忆 | M1 已实现（Goal、Task 与实例意图：创建取 0，其余取所指对象的当前版本；派生 Task 与跳过 occurrence 不提升 Goal 的版本，跳过使 Goal 失败时除外，ADR-0023；上限检查覆盖一个意图连带改变的全部 Goal、Task 与实例，ADR-0018；Kernel 自身的失效会使版本越过最大值时放弃该失效，Goal 保持原状，ADR-0020；策略的结束意图取快照中 Task 的版本，版本不符或结束会使 Task、Goal 或后代的版本越过最大值时 Task 停下并记原因，ADR-0022；结束 Goal 的 `TASK_CANCEL` 的上限检查覆盖 Goal 的结束，结束会使版本越过最大值的跳过只跳过，ADR-0023）：`WorkModel.stale_revision_is_a_conflict_and_changes_nothing`、`GoalTable.stale_revision_is_a_conflict_before_the_state_is_checked`、`GoalTable.change_past_the_largest_revision_is_refused`、`WorkModel.change_past_the_largest_revision_is_refused_and_changes_nothing`、`WorkTask.task_cancel_checks_the_task_then_its_revision_then_its_state`、`WorkTask.instance_intents_check_the_instance_then_its_revision_then_its_state`、`WorkTask.change_that_would_exhaust_a_task_revision_is_refused`、`WorkTask.resume_that_would_exhaust_a_task_revision_is_refused`、`WorkTask.stop_that_would_exhaust_a_goal_revision_is_refused`、`WorkTask.deriving_keeps_the_goal_revision_the_caller_holds`、`TaskTable.revision_is_checked_before_the_state_and_never_wraps`、`AgentTable.revision_is_checked_before_the_state_and_never_wraps`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkExpiry.expiry_that_would_exhaust_a_revision_is_dropped`、`GoalTable.skips_and_dropped_expiries_keep_the_revision`、`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkDecision.end_that_would_exhaust_a_revision_holds_the_task`、`WorkTask.task_cancel_that_would_exhaust_a_revision_its_goals_end_raises_is_refused`、`WorkWait.skip_whose_end_would_exhaust_a_revision_leaves_the_goal_active`；策略意图的依赖版本随步骤与调用（`MA-008`）；E1 步骤 9；E4 步骤 4 |
| C-COMMIT-013 | 外部副作用不参加本地原子事务；通过持久待派发记录、屏障与执行回执对账 | 把“发送通知”当作批次中的一条记录，认为回滚批次就撤销了通知 | E2；评审 |
| C-COMMIT-014 | 跨存储介质（如 NVS 与日志分区）的原子性须有可验证的协调日志或等价协议，不能假定不同分区天然原子 | 预算在 NVS、Task 在日志分区，掉电后只更新了一边 | `MA-010` 故障注入 |
| C-COMMIT-015 | 存储没有空间保存动作屏障时拒绝新的副作用（`AYNI_STORAGE_FAILED`），不跳过屏障 | 日志满时为了不影响体验跳过屏障直接执行 | E2 末段；§10.6 存储行 |
| C-COMMIT-016 | `transition_id` 与 `commit_seq` 在 `durable_seq` 覆盖后才跨启动稳定：没有存下的 transition 不在日志中，之后的启动从自己的启动批次之后重新编号，把同一位置交给另一个 transition。因此高于 `durable_seq` 的值只能与同一次启动的 `durable_seq` 比较，跨启动识别对象使用带代际的 ID（C-ID-010） | 调用方记下重启前一个 Goal 的 `commit_seq`，重启后看到 `durable_seq` 达到该值，就认为那个 Goal 已持久 | M1 已实现：`WorkModel.commit_seq_of_unstored_work_is_reused_by_later_boots`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`（断电时尚未存储的 transition 之后的位置由下一次启动重新使用，ADR-0026）；对外输出的 `requires_seq` 门控随产生对外输出的包 |
| C-OUT-001 | 恢复只重建状态，不直接重放硬件调用；恢复目标也不恢复旧运动命令 | 重启后按日志重新发出最后一条转向命令 | E2 总通过标准 |
| C-OUT-002 | 屏障提交后、真正发出前重启，也只能保守判为 `RESULT_UNKNOWN`；只有可证明没有越过屏障或有权威未执行证据的记录才可重新评估 | 屏障已提交但计数器为 0，就当作未派发直接重做 | E2 F3 |
| C-OUT-003 | Host `submit` 区分“确定拒绝且未保留工作”与“已受理/受理结果未知”；后两者都算在途，Host 内部排队不算确定未派发 | Host 队列中的请求被当成未派发，撤销后不做任何处理 | E2 F10；C-HOST-004 |
