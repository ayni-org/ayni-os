# 契约追踪矩阵

本文件由 `tools/check_contracts.py --write` 根据各契约文档生成，请勿手工编辑。修改条款后重新生成；本文件过期时 CTest 用例 `check_contracts_repository` 失败。

## 概览

| 文档 | 条款数 | 已实现 |
|---|---:|---:|
| [domain/authority_and_trust.md](domain/authority_and_trust.md) | 21 | 1 |
| [domain/calls_and_bindings.md](domain/calls_and_bindings.md) | 19 | 2 |
| [domain/commit_and_output.md](domain/commit_and_output.md) | 19 | 5 |
| [domain/ids_and_time.md](domain/ids_and_time.md) | 18 | 7 |
| [domain/lifecycles.md](domain/lifecycles.md) | 17 | 11 |
| [domain/limits.md](domain/limits.md) | 12 | 6 |
| [domain/permits_and_stop.md](domain/permits_and_stop.md) | 27 | 0 |
| [domain/primitives.md](domain/primitives.md) | 8 | 0 |
| [domain/product_semantics.md](domain/product_semantics.md) | 28 | 0 |
| [domain/resources_and_budget.md](domain/resources_and_budget.md) | 23 | 1 |
| [domain/status_codes.md](domain/status_codes.md) | 7 | 3 |
| [domain/versions.md](domain/versions.md) | 17 | 4 |
| [host/host_ports.md](host/host_ports.md) | 21 | 13 |
| [host/policy_interface.md](host/policy_interface.md) | 11 | 7 |
| [host/provider.md](host/provider.md) | 14 | 0 |
| [storage/records.md](storage/records.md) | 14 | 7 |
| [wire/envelope.md](wire/envelope.md) | 16 | 0 |
| [wire/json_subset.md](wire/json_subset.md) | 12 | 0 |
| 合计 | 304 | 67 |

## 计划章节 → 契约文档

| 计划章节 | 契约文档 |
|---|---|
| §0 | domain/product_semantics.md |
| §2.1 | host/host_ports.md |
| §2.2 | domain/authority_and_trust.md、domain/ids_and_time.md、domain/permits_and_stop.md、domain/primitives.md、domain/resources_and_budget.md |
| §2.3 | domain/primitives.md、domain/resources_and_budget.md |
| §2.4 | domain/commit_and_output.md、domain/lifecycles.md、domain/permits_and_stop.md、domain/product_semantics.md |
| §2.5 | domain/authority_and_trust.md、host/policy_interface.md |
| §2.7 | domain/calls_and_bindings.md、domain/ids_and_time.md、domain/limits.md、domain/primitives.md、host/policy_interface.md、host/provider.md |
| §2.8 | domain/commit_and_output.md、storage/records.md |
| §2.10 | domain/permits_and_stop.md |
| §3.1 | domain/versions.md、wire/envelope.md |
| §3.2 | domain/calls_and_bindings.md、domain/lifecycles.md、domain/limits.md、domain/status_codes.md、domain/versions.md、host/host_ports.md、host/provider.md |
| §3.3 | host/policy_interface.md、host/provider.md |
| §3.4 | domain/authority_and_trust.md、domain/ids_and_time.md、domain/limits.md、domain/status_codes.md、wire/envelope.md、wire/json_subset.md |
| §3.5 | domain/authority_and_trust.md、wire/envelope.md |
| §3.6 | domain/calls_and_bindings.md、domain/ids_and_time.md、domain/lifecycles.md、domain/primitives.md、host/provider.md、wire/envelope.md、wire/json_subset.md |
| §4.1 | domain/permits_and_stop.md、host/host_ports.md |
| §4.2 | domain/limits.md、host/host_ports.md、storage/records.md |
| §4.3 | domain/calls_and_bindings.md、domain/commit_and_output.md、domain/lifecycles.md、host/host_ports.md、storage/records.md |
| §4.4 | domain/ids_and_time.md、domain/lifecycles.md、domain/product_semantics.md、domain/versions.md、storage/records.md |
| §5.1 | domain/resources_and_budget.md、host/provider.md |
| §5.2 | domain/product_semantics.md |
| §5.3 | domain/limits.md、domain/resources_and_budget.md、host/policy_interface.md |
| §5.4 | domain/authority_and_trust.md、domain/product_semantics.md |
| §7.3 | domain/permits_and_stop.md |
| §10.6 | domain/limits.md、wire/json_subset.md |
| §10.7 | domain/product_semantics.md |
| §14.1 | domain/product_semantics.md |

## ADR → 契约文档

| ADR | 契约文档 |
|---|---|
| ADR-0001 | domain/permits_and_stop.md、domain/primitives.md、domain/resources_and_budget.md |
| ADR-0002 | domain/commit_and_output.md、host/host_ports.md、storage/records.md |
| ADR-0003 | domain/authority_and_trust.md |
| ADR-0004 | domain/authority_and_trust.md、domain/calls_and_bindings.md、domain/ids_and_time.md、domain/versions.md、wire/envelope.md |
| ADR-0005 | domain/permits_and_stop.md |
| ADR-0006 | domain/lifecycles.md、domain/status_codes.md、host/host_ports.md、host/policy_interface.md、host/provider.md |
| ADR-0007 | domain/calls_and_bindings.md、domain/lifecycles.md、domain/primitives.md、domain/product_semantics.md |
| ADR-0008 | domain/primitives.md、domain/resources_and_budget.md |
| ADR-0009 | domain/versions.md、wire/envelope.md、wire/json_subset.md |
| ADR-0013 | domain/versions.md |
| ADR-0014 | domain/limits.md、host/host_ports.md |
| ADR-0015 | domain/limits.md、host/host_ports.md |
| ADR-0016 | domain/lifecycles.md、domain/limits.md、host/host_ports.md、storage/records.md |
| ADR-0017 | domain/authority_and_trust.md、domain/commit_and_output.md、domain/ids_and_time.md、domain/lifecycles.md、domain/limits.md、domain/primitives.md、domain/status_codes.md、host/policy_interface.md |
| ADR-0018 | domain/commit_and_output.md、domain/ids_and_time.md、domain/lifecycles.md、domain/limits.md、domain/primitives.md、domain/status_codes.md、host/policy_interface.md |
| ADR-0019 | domain/ids_and_time.md、domain/lifecycles.md、domain/limits.md、domain/primitives.md、domain/product_semantics.md、host/host_ports.md、host/policy_interface.md |
| ADR-0020 | domain/commit_and_output.md、domain/ids_and_time.md、domain/lifecycles.md、domain/limits.md、domain/primitives.md、domain/product_semantics.md、domain/resources_and_budget.md、domain/status_codes.md、host/host_ports.md、host/policy_interface.md |
| ADR-0021 | domain/calls_and_bindings.md、domain/lifecycles.md、domain/limits.md、domain/primitives.md、host/host_ports.md、host/policy_interface.md |
| ADR-0022 | domain/commit_and_output.md、domain/lifecycles.md、domain/limits.md、domain/status_codes.md、host/policy_interface.md |
| ADR-0024 | host/policy_interface.md |
| ADR-0026 | domain/calls_and_bindings.md、domain/commit_and_output.md、domain/ids_and_time.md、domain/lifecycles.md、domain/limits.md、domain/product_semantics.md、domain/status_codes.md、domain/versions.md、host/host_ports.md、host/policy_interface.md、storage/records.md、wire/envelope.md |

## 工作包 → 条款

| 工作包 | 条款 |
|---|---|
| `MA-001` | C-STOP-003、C-STOP-004 |
| `MA-003` | C-LIMIT-001、C-LIMIT-008、C-LIMIT-011、C-STOP-001、C-HOST-002、C-HOST-003、C-HOST-006、C-HOST-018 |
| `MA-004` | C-AUTH-004、C-WIRE-014 |
| `MA-005` | C-LIFE-002、C-LIFE-005、C-LIFE-006、C-LIFE-007、C-LIMIT-002、C-STATUS-001、C-STATUS-003、C-STATUS-006、C-HOST-015、C-HOST-018、C-PROV-001、C-WIRE-014、C-JSON-009、C-JSON-012 |
| `MA-006` | C-AUTH-001、C-AUTH-003、C-AUTH-004、C-AUTH-008、C-AUTH-010、C-POLICY-003 |
| `MA-007` | C-HOST-002、C-HOST-007、C-HOST-008、C-HOST-015、C-HOST-018、C-HOST-020、C-HOST-021、C-WIRE-001、C-WIRE-002、C-WIRE-003、C-WIRE-013、C-WIRE-014、C-JSON-001、C-JSON-002、C-JSON-003、C-JSON-004、C-JSON-009 |
| `MA-008` | C-CALL-001、C-CALL-003、C-CALL-004、C-BIND-002、C-BIND-003、C-COMMIT-012、C-LIFE-003、C-LIFE-021、C-LIFE-022、C-BUDGET-005、C-VER-030、C-POLICY-008、C-POLICY-009、C-PROV-002、C-PROV-003、C-PROV-010 |
| `MA-009` | C-LIMIT-005、C-LIMIT-006、C-POLICY-001、C-POLICY-002、C-POLICY-005、C-POLICY-008、C-POLICY-011 |
| `MA-010` | C-COMMIT-003、C-COMMIT-014、C-ID-009、C-VER-020、C-VER-023、C-HOST-009、C-HOST-011、C-STORE-001、C-STORE-003、C-STORE-006、C-STORE-007 |
| `MA-011` | C-ID-001、C-TIME-004、C-TIME-005、C-TIME-006、C-TIME-007、C-LIMIT-012、C-PROD-018、C-PROD-032、C-PROD-034、C-PROD-042、C-POLICY-009、C-JSON-010 |
| `MA-012` | C-LIMIT-010 |
| `MA-015` | C-PRIM-005、C-PROV-013、C-PROV-014、C-WIRE-011 |
| `MA-016` | C-AUTH-002、C-AUTH-012、C-AUTH-015、C-LIFE-020、C-PRIM-007、C-VER-010、C-WIRE-003、C-WIRE-006、C-WIRE-007、C-WIRE-009、C-WIRE-010 |
| `MA-018` | C-AUTH-016、C-SUB-004、C-LIFE-021、C-LIFE-022、C-LIFE-023、C-LIMIT-002、C-PERMIT-001、C-PERMIT-003、C-PERMIT-016、C-PRIM-001、C-PRIM-008、C-RES-001、C-RES-003、C-RES-005、C-RES-006、C-RES-012、C-RES-013、C-VER-030、C-POLICY-003、C-POLICY-009 |
| `MA-019` | C-LIFE-023、C-LIMIT-007、C-PROD-012、C-PROD-015、C-PROD-018、C-PROD-050 |
| `MA-026` | C-AUTH-011、C-PERMIT-010、C-PERMIT-011、C-PERMIT-012、C-PERMIT-016、C-PERMIT-017、C-STOP-002、C-STOP-003、C-STOP-004、C-STOP-005、C-STOP-007、C-STOP-008、C-STOP-009、C-STOP-010、C-STORE-012 |
| `MA-027` | C-PROD-033、C-BUDGET-010、C-PROV-011 |
| `MA-028` | C-PROD-001、C-PROD-003、C-PROD-004、C-PROD-051 |
| `MA-030` | C-WIRE-015 |
| `MA-034` | C-TRUST-002 |
| `MA-035` | C-PERMIT-014、C-JSON-011 |
| `MA-037` | C-TRUST-005 |
| `MA-038` | C-AUTH-012、C-AUTH-013、C-ID-005、C-TIME-002、C-PERMIT-001、C-PERMIT-015、C-PERMIT-017、C-RES-012、C-WIRE-005、C-WIRE-006、C-WIRE-008、C-WIRE-009、C-WIRE-014、C-JSON-010 |

## 实验步骤 → 条款

| 实验步骤 | 条款 |
|---|---|
| E1 步骤 1 | C-PRIM-007、C-PROD-041、C-PROD-042、C-WIRE-010 |
| E1 步骤 2 | C-RES-009、C-RES-011、C-PROV-012 |
| E1 步骤 3 | C-RES-001、C-RES-002、C-RES-004、C-RES-006、C-RES-009、C-RES-010 |
| E1 步骤 4 | C-CALL-009、C-LIFE-012、C-STOP-006、C-PRIM-004、C-RES-007、C-RES-008、C-HOST-005、C-PROV-006 |
| E1 步骤 5 | C-AUTH-004、C-LIMIT-005、C-RES-003、C-BUDGET-001、C-BUDGET-003、C-BUDGET-004、C-BUDGET-007、C-BUDGET-008 |
| E1 步骤 6 | C-AUTH-001、C-AUTH-002、C-AUTH-004、C-AUTH-005、C-AUTH-011、C-LIFE-013 |
| E1 步骤 7 | C-AUTH-009、C-AUTH-010、C-TRUST-001、C-PROD-002、C-PROD-035、C-PROD-040、C-PROD-041 |
| E1 步骤 8 | C-SUB-001、C-SUB-002、C-SUB-003、C-LIMIT-009、C-PRIM-005、C-PROD-023、C-RES-010 |
| E1 步骤 9 | C-CALL-005、C-COMMIT-012、C-ID-001、C-PROD-030、C-PROD-031、C-BUDGET-009、C-PROV-003、C-PROV-007 |
| E1 步骤 10 | C-CALL-012、C-SUB-004、C-LIMIT-008、C-STOP-001、C-RES-004、C-RES-005、C-BUDGET-006、C-HOST-006、C-WIRE-013 |
| E1 步骤 11 | C-CALL-003、C-TIME-005、C-LIFE-023、C-STOP-002、C-PROD-010、C-PROD-012、C-PROD-013、C-PROD-014、C-PROD-016、C-PROD-018、C-PROD-020、C-PROD-021 |
| E2 F0 | C-STORE-002 |
| E2 F1 | C-AUTH-005、C-AUTH-007、C-TIME-003、C-PRIM-001、C-PRIM-006 |
| E2 F2 | C-COMMIT-006、C-PERMIT-002、C-PRIM-001 |
| E2 F3 | C-OUT-002、C-LIFE-010、C-PRIM-001、C-STATUS-004 |
| E2 F4 | C-LIFE-010、C-PRIM-001、C-STATUS-004 |
| E2 F5 | C-ID-004、C-LIFE-010、C-BUDGET-003、C-STATUS-004 |
| E2 F6 | C-COMMIT-004、C-WIRE-007 |
| E2 F7 | C-CALL-002、C-CALL-011、C-COMMIT-002、C-ID-006、C-HOST-003、C-HOST-009、C-HOST-011、C-STORE-001、C-STORE-002、C-STORE-009 |
| E2 F8 | C-COMMIT-002、C-COMMIT-005、C-LIMIT-007、C-BUDGET-002、C-HOST-009、C-STORE-002 |
| E2 F9 | C-COMMIT-004、C-ID-007、C-WIRE-004、C-WIRE-005、C-WIRE-016 |
| E2 F10 | C-AUTH-005、C-AUTH-006、C-COMMIT-007、C-OUT-003、C-LIFE-013、C-PERMIT-002、C-PRIM-006、C-HOST-004 |
| E2 F11 | C-CALL-012、C-COMMIT-005、C-BUDGET-002、C-STORE-007 |
| E2 F12 | C-COMMIT-008、C-COMMIT-009、C-COMMIT-010、C-COMMIT-011、C-ID-009、C-PERMIT-009、C-HOST-012、C-STORE-005 |
| E2 F13 | C-LIFE-004 |
| E2 F14 | C-TRUST-004、C-PERMIT-004、C-PERMIT-005、C-PERMIT-006、C-PERMIT-007、C-PERMIT-008、C-PERMIT-012、C-PERMIT-013 |
| E3 步骤 1 | C-POLICY-006、C-PROV-012 |
| E3 步骤 2 | C-CALL-002、C-CALL-008、C-CALL-011、C-ID-003、C-ID-004、C-ID-006、C-POLICY-003、C-POLICY-006、C-POLICY-010、C-PROV-012 |
| E3 步骤 3 | C-ID-003、C-POLICY-006、C-PROV-012 |
| E3 步骤 4 | C-CALL-004、C-ID-003、C-POLICY-006、C-POLICY-008、C-POLICY-010、C-PROV-012 |
| E3 步骤 5 | C-CALL-004、C-LIFE-024、C-VER-030、C-POLICY-006、C-POLICY-007、C-PROV-012 |
| E3 步骤 6 | C-PROV-011 |
| E4 步骤 1 | C-CALL-001、C-BIND-001、C-POLICY-003、C-PROV-009、C-JSON-012 |
| E4 步骤 2 | C-CALL-004、C-CALL-007、C-ID-003、C-POLICY-008 |
| E4 步骤 3 | C-CALL-006、C-CALL-008、C-CALL-009、C-LIFE-011、C-LIFE-022、C-LIMIT-006 |
| E4 步骤 4 | C-SUB-002、C-COMMIT-012、C-PROD-030 |
| E4 步骤 5 | C-VER-011、C-VER-030、C-POLICY-009 |
| E4 步骤 6 | C-VER-020、C-VER-021、C-VER-022、C-VER-031、C-STORE-004、C-STORE-010 |
| E4 步骤 7 | C-PROD-010、C-PROD-011、C-PROD-013、C-PROD-015、C-PROD-017 |

## 已实现的条款

| 条款 | 验证 |
|---|---|
| C-AUTH-010 | M1 已实现（范围：意图只带 `session_id`，主体、渠道与来源取自会话，意图不能自报；`ayni_open_session` 仍是任何调用方都能调用的公开函数，只许 Host 打开会话随 `MA-006` 与组合根）：`WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`、`WorkModel.intent_through_a_closed_or_unknown_session_is_denied`、`WorkModel.created_goal_is_applied_at_once_and_not_yet_durable`、`SessionTable.closed_session_is_gone_and_frees_its_place`；渠道信任级别对授权的影响随 `MA-006`；E1 步骤 7 |
| C-CALL-004 | M1 已实现（已提交进度：步骤由 Kernel 保存，策略只能以 `REPORT_PROGRESS` 追加；私有检查点与步骤分开，只保存策略返回的状态，ADR-0024）：`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`、`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`、`TaskTable.decisions_commit_steps_checkpoints_and_progress`；步骤版本、绑定、输入摘要、结果引用与未知状态随 `MA-008`，共同检查点的版本不兼容时暂停也随之；重启后私有检查点不兼容的 Task 停下，步骤保留（ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`；E3 步骤 4–5；E4 步骤 2 |
| C-SUB-004 | M1 已实现（本地输入不合并、不丢弃，队列满时返回 `BUSY`，ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`IngressQueue.full_queue_rejects_other_events_with_busy`；遥测的合并与丢弃见 C-LIMIT-008，订阅事件的 gap 随 `MA-018`；E1 步骤 10；C-HOST-006 |
| C-COMMIT-001 | M1 已实现（Kernel 只在追加结果为 `OK` 时推进 `durable_seq`，`FAILED`、`UNKNOWN` 与被拒绝的请求使它受限；Native Host 在 `fdatasync` 成功后才报告 `OK`，ADR-0026）：`WorkJournal.result_is_durable_once_the_host_stores_its_batch`、`WorkJournal.append_that_fails_restricts_the_kernel`、`HostStorage.append_then_read_returns_the_same_bytes`；C-HOST-010 |
| C-COMMIT-002 | M1 已实现（工作模型：恢复按序重放每个已存储批次各一次，追加中断电的批次只在完整写入时计入，ADR-0026）：`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`、`HostStorage.torn_tail_is_dropped_and_overwritten`；E2 F7、F8 |
| C-COMMIT-004 | M1 已实现（查询标注：对象带 `commit_seq`，`ayni_get_kernel_info` 报告 `durable_seq`，结果未知的追加不计入）：`KernelLifecycle.durable_seq_follows_the_log_read_and_then_the_boot_batch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`WorkModel.created_goal_is_applied_at_once_and_not_yet_durable`；`durable_seq` 推进到 Host 已存储的批次，步骤报告也给出它（ADR-0026）：`WorkJournal.result_is_durable_once_the_host_stores_its_batch`、`KernelLifecycle.step_reports_durable_seq_when_the_report_covers_it`；对外输出的 `requires_seq` 门控随产生对外输出的包；E2 F6、F9；属性测试：每个已释放输出的 seq ≤ 恢复后的 `durable_seq` |
| C-COMMIT-012 | M1 已实现（Goal、Task 与实例意图：创建取 0，其余取所指对象的当前版本；派生 Task 与跳过 occurrence 不提升 Goal 的版本，跳过使 Goal 失败时除外，ADR-0023；上限检查覆盖一个意图连带改变的全部 Goal、Task 与实例，ADR-0018；Kernel 自身的失效会使版本越过最大值时放弃该失效，Goal 保持原状，ADR-0020；策略的结束意图取快照中 Task 的版本，版本不符或结束会使 Task、Goal 或后代的版本越过最大值时 Task 停下并记原因，ADR-0022；结束 Goal 的 `TASK_CANCEL` 的上限检查覆盖 Goal 的结束，结束会使版本越过最大值的跳过只跳过，ADR-0023）：`WorkModel.stale_revision_is_a_conflict_and_changes_nothing`、`GoalTable.stale_revision_is_a_conflict_before_the_state_is_checked`、`GoalTable.change_past_the_largest_revision_is_refused`、`WorkModel.change_past_the_largest_revision_is_refused_and_changes_nothing`、`WorkTask.task_cancel_checks_the_task_then_its_revision_then_its_state`、`WorkTask.instance_intents_check_the_instance_then_its_revision_then_its_state`、`WorkTask.change_that_would_exhaust_a_task_revision_is_refused`、`WorkTask.resume_that_would_exhaust_a_task_revision_is_refused`、`WorkTask.stop_that_would_exhaust_a_goal_revision_is_refused`、`WorkTask.deriving_keeps_the_goal_revision_the_caller_holds`、`TaskTable.revision_is_checked_before_the_state_and_never_wraps`、`AgentTable.revision_is_checked_before_the_state_and_never_wraps`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkExpiry.expiry_that_would_exhaust_a_revision_is_dropped`、`GoalTable.skips_and_dropped_expiries_keep_the_revision`、`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkDecision.end_that_would_exhaust_a_revision_holds_the_task`、`WorkTask.task_cancel_that_would_exhaust_a_revision_its_goals_end_raises_is_refused`、`WorkWait.skip_whose_end_would_exhaust_a_revision_leaves_the_goal_active`；策略意图的依赖版本随步骤与调用（`MA-008`）；E1 步骤 9；E4 步骤 4 |
| C-COMMIT-016 | M1 已实现：`WorkModel.commit_seq_of_unstored_work_is_reused_by_later_boots`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`（断电时尚未存储的 transition 之后的位置由下一次启动重新使用，ADR-0026）；对外输出的 `requires_seq` 门控随产生对外输出的包 |
| C-ID-001 | M1 已实现（Kernel 分配的 ID 不复用，断电时尚未存储的对象的 ID 也不再分配；让位的 Goal 与 Task 按旧 ID 返回 `NOT_FOUND`）：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`、`WorkTask.goal_that_ended_first_gives_way_with_its_tasks`；记忆删除的 tombstone 随 `MA-011`；E1 步骤 9 |
| C-ID-006 | M1 已实现（完成槽）：`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`；恢复请求以 `boot_epoch` 0 命名且 `request_id` 依次递增：`KernelLifecycle.recovery_requests_use_boot_epoch_0`；Host 不投递旧实例的结果（ADR-0026）：`HostStorage.new_host_instance_never_posts_an_earlier_request`；E2 F7；E3 步骤 2 |
| C-ID-009 | M1 已实现（启动）：`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`、`KernelLifecycle.each_reboot_persists_the_next_epoch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.reboot_after_an_unknown_append_skips_its_epoch`；受限栅栏重载随 `MA-010`；E2 F12 |
| C-ID-010 | M1 已实现：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`、`KernelLifecycle.largest_epoch_and_transition_id_still_boot`、`WorkTask.once_goal_derives_one_task_in_the_next_step`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`；实例跨重启保持同一 `instance_id`（ADR-0026）：`WorkRecovery.instance_of_an_agent_no_longer_registered_is_faulted` |
| C-TIME-001 | M1 已实现（step 时间预算、触发时间、Goal 失效与 occurrence 的等待上限）：`KernelLifecycle.step_returns_once_its_time_budget_is_spent`、`WorkTrigger.delay_goal_derives_once_its_time_comes`、`WorkTrigger.interval_goal_derives_each_occurrence_on_its_schedule`、`WorkTrigger.step_takes_occurrences_as_of_its_start`、`WorkExpiry.goal_expires_at_its_time`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`；TTL、租期与 Task 执行中的等待上限随各自工作包 |
| C-TIME-003 | M1 已实现（工作模型：Goal 的失效与触发只保存时长，Task 不保存 `due_us`，恢复时一律“从恢复时刻重新计时”，ADR-0026 决策 6）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`WorkRecovery.input_goal_takes_inputs_after_a_restart`、`WorkRecords.records_hold_no_time_of_the_boot_that_wrote_them`、`GoalRestore.reanchor_restarts_every_wait_in_full_at_recovery`、`TaskRestore.reanchor_makes_every_task_due_at_recovery`；Observation、租期等其余期限随各自工作包；E2 F1 |
| C-TIME-008 | M0 已实现：`NativeHost.monotonic_clock_never_decreases`（Native）；ESP32 使用 `esp_timer_get_time`，M2 补测 |
| C-LIFE-001 | M0 已实现：`KernelApi.create_in_caller_memory_starts_configuring` |
| C-LIFE-002 | M1 已实现：`KernelApi.kernel_memory_is_reusable_after_destroy`、`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`；`MA-005` |
| C-LIFE-003 | M1 已实现（注册只在 `CONFIGURING`；`ayni_start` 解析 GoalType 指定的策略，缺少时返回 `NOT_FOUND` 并保持 `CONFIGURING`，补登记后可再启动，ADR-0022）：`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.return_values_follow_the_state_table`、`WorkStrategy.start_resolves_strategy_ids_and_waits_for_a_missing_one`、`GoalTypeRegistry.link_strategies_fails_on_a_missing_strategy_and_may_run_again`；其余交叉引用随引用其他目录项的注册（绑定：`MA-008`） |
| C-LIFE-004 | M1 已实现：`WorkModel.intent_outside_running_is_not_ready`、`WorkModel.draining_answers_queries_and_closes_sessions_but_takes_no_work`、`WorkModel.return_values_follow_the_state_table`；E2 F13 |
| C-LIFE-007 | M1 已实现：`KernelLifecycle.return_values_follow_the_state_table`、`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.restriction_is_reported_only_while_restricted`；`MA-005` |
| C-LIFE-020 | M1 已实现（Goal：创建时指定本 Kernel 的实例，之后没有改变 owner 的意图；Task 继承其 Goal 的 owner，ADR-0018）：`WorkModel.owner_must_be_an_instance_of_this_kernel`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.child_must_have_its_parents_owner`、`WorkTask.once_goal_derives_one_task_in_the_next_step`、`WorkTask.instance_pause_holds_its_tasks_and_resume_releases_them`；跨重启（owner 随 Goal 与 Task 记入日志；本次没有注册的 Agent 的实例 `FAULTED`，名下工作停下而不转交，ADR-0026 决策 7）：`WorkRecovery.instance_of_an_agent_no_longer_registered_is_faulted`、`WorkReplay.goal_of_an_instance_no_record_started_is_corrupt`；`MA-016` |
| C-LIFE-021 | M1 已实现（Goal 状态与版本；暂停的 Goal 及其后代不派生 Task，已有的 Task 停下，ADR-0018）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.pause_and_resume_change_only_the_goal`、`GoalTable.lifecycle_decides_which_changes_are_allowed`、`WorkTask.paused_ancestor_holds_the_tasks_below_it_until_resumed`、`WorkTask.goal_created_under_a_paused_parent_derives_once_the_parent_resumes`、`WorkTask.paused_goal_holds_its_own_task`、`WorkTask.task_waits_for_both_its_goal_and_its_owner`；暂停与恢复保留 Task 的步骤与私有检查点，恢复后的决定从它们开始（ADR-0024）：`WorkCheckpoint.pause_keeps_the_steps_and_checkpoint_for_the_next_decision`；在途 Task 的暂停点随 `MA-008`、`MA-018`（ADR-0022） |
| C-LIFE-022 | M1 已实现（Goal 及其后代与它们的 Task 在一个 transition 中取消并记录 `cancel_revision`；取消的 Goal 不再派生 Task；取消 Goal 的最后一个 Task 而它不会再有 occurrence 时 Goal 随之取消，ADR-0023）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.cancel_reaches_every_live_descendant_and_nothing_else`、`WorkTask.cancel_reaches_the_tasks_of_the_goal_and_its_descendants`、`WorkTask.task_cancel_of_a_one_shot_goals_task_cancels_the_goal`、`WorkTask.task_cancel_cancels_a_one_shot_goal_whatever_holds_its_task`、`WorkTask.task_cancel_ends_a_goal_only_with_its_last_task`、`WorkTask.task_cancel_ends_an_interval_goal_whose_next_time_never_comes`、`WorkInput.task_cancel_leaves_the_goal_taking_inputs`、`TaskTable.pause_resume_and_cancel_follow_the_task_lifecycle`，子目标排在父目标之前时同样（ADR-0025）`WorkTask.child_in_an_earlier_place_follows_its_parent`；传播到在途请求随 `MA-008`、`MA-018`（ADR-0022）；E4 步骤 3 |
| C-LIFE-023 | M1 已实现（实例暂停使名下 Task 停下、恢复后继续，暂停中的实例仍接收新 Goal 而不派生；停止在一个 transition 中取消名下全部未终结的 Goal 与 Task，ADR-0018）：`WorkTask.instance_pause_holds_its_tasks_and_resume_releases_them`、`WorkTask.paused_instance_takes_goals_and_derives_after_resume`、`WorkTask.held_goal_keeps_no_later_goal_from_deriving`、`WorkTask.task_waits_for_both_its_goal_and_its_owner`、`WorkTask.instance_stop_cancels_its_goals_and_tasks`、`AgentTable.pause_resume_and_stop_follow_the_instance_lifecycle`；实例暂停保留 Task 的步骤与私有检查点（ADR-0024）：`WorkCheckpoint.pause_keeps_the_steps_and_checkpoint_for_the_next_decision`；重启后检查点不兼容时停下的是 Task 而不是实例（ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`；资源回收随 `MA-018`；“暂停助理”的集合随 `MA-019`；E1 步骤 11 |
| C-LIFE-024 | M1 已实现（恢复时检查点不是当前策略及其版本写的、GoalType 不再有策略或其版本缺失时，Task 停下并报告原因；只在内存中，步骤、检查点、完成条件与版本不变，注册了原策略的启动从检查点继续，ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`、`WorkRecovery.goal_type_this_boot_lacks_holds_its_goals_and_tasks`、`TaskRestore.hold_restored_holds_a_live_task_at_its_revision`；E3 步骤 5 |
| C-LIFE-025 | M1 已实现（首次进入 `RUNNING` 时）：`WorkModel.instances_start_with_the_kernel_in_one_transition`、`AgentTable.start_all_gives_each_instance_an_id_and_the_stamp`、`AgentTable.registers_each_agent_once_up_to_capacity`；M1 已实现（重启后：实例记入日志，`instance_id` 与版本不变，启动 transition 只启动首次注册的 Agent 的实例，ADR-0026 决策 8）：`WorkRecovery.agent_registered_for_the_first_time_starts_after_the_logged_ones`、`AgentRestore.registered_agent_gets_its_logged_instance`；策略尚无初始化，不经过 `STARTING` |
| C-LIMIT-001 | M1 已实现（入口、完成槽、存储缓冲与工作模型）：`KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`、`IngressQueue.ten_thousand_events_pass_in_order_without_loss`、`KernelApi.capacity_outside_its_range_is_invalid_argument`、`KernelApi.memory_requirements_grow_with_each_capacity`、`KernelApi.goal_tree_limits_need_no_memory`、`WorkModel.full_goal_table_refuses_without_side_effects`、`WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`；超过等待上限的 occurrence 带计数与原因跳过，不是静默丢弃（ADR-0020）：`WorkWait.occurrences_are_skipped_once_their_wait_ends`；恢复时本次容量放不下日志中的状态，以 `STATE_EXCEEDS_CAPACITY` 受限而不丢弃其中的工作（ADR-0026 决策 5）：`WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`、`GoalRestore.restore_reports_what_this_boots_limits_cannot_hold`；其余容量随各自工作包；`MA-003`；第 10.6 节 |
| C-LIMIT-002 | M1 已实现（已有的限额）：入口 `IngressQueue.full_queue_rejects_other_events_with_busy`；完成槽 `CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`；存储缓冲 `KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`；主体、Agent 与 GoalType `WorkModel.registration_refuses_duplicates_and_capacity`；策略 `WorkStrategy.registration_checks_its_arguments`；一次决定的意图 `WorkDecision.refused_decisions_hold_the_task_with_their_reason`；Goal `WorkModel.full_goal_table_refuses_without_side_effects`；Goal 参数 `WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`；子目标 `WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；会话 `WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`；ID `WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`、`WorkWait.used_up_ids_skip_with_their_reason`；Task `WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`TaskTable.full_table_of_live_tasks_refuses_and_changes_nothing`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`；其余限额随各自工作包；`MA-018`；`MA-005` |
| C-LIMIT-004 | M0 已实现：ctest `core_does_not_reference_heap_allocator` 检查每个 Core 静态库不引用 `malloc`/`free`/`operator new`/`operator delete`，也不引用 `strdup`、`asprintf`、`getline` 等替调用方分配内存的 libc 函数；ctest `heap_check_detects_allocator` 与 ctest `heap_check_detects_strdup` 证明该检查有效 |
| C-LIMIT-007 | M1 已实现（子目标深度与数量）：`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.tree_limits_refuse_deeper_or_more_children`、`GoalTable.zero_limits_allow_top_level_goals_only`、`WorkModel.tree_limits_missing_from_the_config_take_their_defaults`，子目标排在父目标之前时同样（ADR-0025）`GoalTable.child_in_an_earlier_place_is_still_in_its_parents_tree`；M1 已实现（本次启动内的去重）：等待的 occurrence 合并为一个 Task，节拍不变（ADR-0019）`TriggerSchedule.waiting_occurrences_are_taken_together_and_the_schedule_holds`、`WorkTrigger.occurrences_missed_between_steps_become_one_task`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`；M1 已实现（重启后：不补建断电期间错过的 occurrence，计时从恢复时刻按完整时长重新开始，ADR-0026 决策 6）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`GoalRestore.reanchor_restarts_every_wait_in_full_at_recovery`；日历提醒的“错过补发一次”随 `MA-019`（C-PROD-018）；E2 F8 |
| C-LIMIT-008 | M1 已实现（入口与完成槽）：`IngressQueue.full_queue_rejects_other_events_with_busy`、`IngressQueue.full_queue_replaces_newest_value_of_the_same_stream`、`IngressQueue.telemetry_flood_keeps_each_stream_in_order_and_accounted`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；本地输入不丢弃、队列满时返回 `BUSY`（ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`WorkInput.small_event_bodies_still_hold_a_source`；持续的事件流不挡到期工作：`WorkInput.steady_events_do_not_hold_back_due_work`；E1 步骤 10；`MA-003` |
| C-LIMIT-011 | M1 已实现：`KernelLifecycle.step_applies_at_most_max_items`、`KernelLifecycle.step_returns_once_its_time_budget_is_spent`、`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`（请求在之后的 step 中完成）、`KernelLifecycle.recovery_waits_for_a_free_slot_without_spinning`（等待空闲槽时不报告 `more_ready`）；策略的决定每个算一个 item，只取本次 step 之前已是 `READY` 的 Task（ADR-0022）：`WorkDecision.step_stops_deciding_once_its_time_budget_is_spent`、`WorkDecision.task_is_decided_in_the_step_after_it_became_ready`；`MA-003`；第 10.6 节「事件与容量」 |
| C-RES-005 | M1 已实现（occurrence 等待 Task 槽位或 ID：GoalType 声明等待上限，超限跳过并计数、记原因码，可声明无界等待；Goal 信息给出当前的等待原因，ADR-0020；有停下的 Task 的 Goal 同样等待并跳过，原因 `TASK_HELD`，ADR-0022；跳过最后一个 occurrence 且没有未结束的 Task 时 Goal 进入 `FAILED`，原因保留在 `skip_reason`，ADR-0023）：`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.used_up_ids_skip_with_their_reason`、`WorkWait.skipping_the_last_occurrence_fails_the_goal`、`WorkWait.skip_leaves_a_goal_with_a_live_task_active`、`WorkWait.zero_wait_skips_at_once`、`WorkWait.unbounded_wait_never_skips`、`WorkWait.wait_reason_reports_what_would_hold_the_next_occurrence`、`WorkDecision.held_task_keeps_its_goal_from_deriving_until_it_ends`、`WorkDecision.held_task_keeps_its_goal_waiting_with_room_in_the_table`；Task 执行中的等待（资源、预算）与延后、暂停两种处理随 `MA-018`（ADR-0022 决策 1）；E1 步骤 10 |
| C-STATUS-001 | M0 已实现：`include/ayni/ayni.h` 为每个已实现函数列出返回码，`tests/unit/kernel_api_test.cpp` 覆盖 `ayni_create` 文档列出的每个失败码；`MA-005` 生命周期返回值表：`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.step_rejects_bad_arguments_without_effect`、`KernelLifecycle.kernel_info_checks_its_output`；工作模型：`WorkModel.return_values_follow_the_state_table`、`WorkModel.intent_arguments_are_refused_before_anything_else`、`WorkModel.registration_checks_its_arguments`、`WorkModel.queries_check_their_arguments`、`WorkTask.task_and_instance_bodies_are_checked_before_anything_else`、`WorkTask.task_query_checks_its_arguments`、`WorkTask.task_and_instance_intents_outside_running_are_not_ready` |
| C-STATUS-002 | M0 已实现：`KernelApi.failed_create_leaves_memory_untouched` |
| C-STATUS-006 | M1 已实现（`ayni_submit_intent`：每次返回都写状态、原因码与对象引用，只含 ID 与版本）：`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written`、`WorkModel.intent_through_a_closed_or_unknown_session_is_denied`、`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；日志与诊断输出的测试随其实现（M1）；`MA-005` |
| C-VER-001 | M0 已实现：`KernelApi.create_rejects_other_abi_major`、`KernelApi.create_rejects_other_abi_minor`、`KernelApi.config_from_abi_0_1_is_unsupported_version`、`KernelApi.failed_create_leaves_memory_untouched` |
| C-VER-002 | M1 已实现：编译期检查 `tests/contract/abi_layout.c` 以 `offsetof` 与 `sizeof` 固定已发布结构体的布局；评审 |
| C-VER-003 | M0 已实现：`KernelApi.create_copies_host_table_before_return`；`KernelApi.config_from_abi_0_1_is_unsupported_version`（`struct_size` 不含 `abi_minor` 时按 0.1 处理）；M1 起另有 `KernelApi.host_table_without_0_3_ports_is_invalid_argument`、`KernelApi.host_table_without_storage_ports_is_accepted`、`KernelApi.uncovered_capacities_take_their_defaults`、`KernelLifecycle.kernel_info_checks_its_output`；0.5 起新增或改变的结构体只有一个布局，只接受自身的 `sizeof`：`WorkModel.registration_checks_its_arguments`（含 0.7 与 0.9 的 `ayni_goal_type_v1` 布局）、`WorkStrategy.registration_checks_its_arguments`、`WorkModel.intent_arguments_are_refused_before_anything_else`（含 0.6、0.7 与 0.8 的 `ayni_goal_create_v1` 布局）、`WorkModel.queries_check_their_arguments`（含 0.7 与 0.8 的 `ayni_goal_info_v1` 布局）、`KernelEvents.post_checks_arguments_kind_and_source_before_the_state`、`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written`、`WorkTask.task_and_instance_bodies_are_checked_before_anything_else`、`WorkTask.task_query_checks_its_arguments`（含 0.9 的 `ayni_task_info_v1` 布局） |
| C-VER-004 | M0 已实现：`KernelApi.struct_size_below_first_layout_is_invalid_argument`、`KernelApi.struct_size_from_newer_minor_is_unsupported_version` |
| C-HOST-001 | M0 已实现：`KernelApi.create_copies_host_table_before_return` |
| C-HOST-004 | M1 已实现（Kernel 侧）：`CompletionPoolTest.rejected_request_frees_the_slot_and_runs_no_handler`、`CompletionPoolTest.accept_unknown_waits_for_the_post`、`CompletionPoolTest.undefined_submit_result_counts_as_in_flight`、`CompletionPoolTest.post_before_a_rejection_is_still_applied`；E2 F10；C-OUT-003 |
| C-HOST-006 | M1 已实现（完成槽部分）：`CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`、`CompletionPoolTest.result_is_applied_once`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`、`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionPoolTest.concurrent_posts_apply_each_result_exactly_once`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；遥测合并见 C-LIMIT-008，订阅的 gap 报告随订阅实现；E1 步骤 10；`MA-003` |
| C-HOST-007 | M1 已实现：`NativeHost.wakes_from_other_threads_merge_into_one`、`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`；ESP-IDF Host 随 `MA-007` 复核 |
| C-HOST-009 | M1 已实现（`append` 与 `read_prefix`：Kernel 侧，以及模拟与 Native Host 的分帧、按日志顺序只返回完整批次、丢弃撕裂的尾部，ADR-0026）：`KernelApi.create_rejects_only_one_storage_port`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`、`HostStorage.reads_follow_log_order_whatever_the_commit_seq`、`HostStorage.only_whole_batches_fit_the_buffer`、`HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.log_file_frames_each_batch_with_two_crcs`；栅栏、快照与 `storage_free_bytes` 端口随 `MA-010` 的 E、F 部分；E2 F7、F8 |
| C-HOST-010 | M1 已实现（Native Host：追加在 `fdatasync` 成功后才报告 `OK`，同步失败报告 `UNKNOWN` 且之后的追加都失败，ADR-0026）：评审 `hosts/native/file_log.cpp`，`HostStorage.batches_survive_a_new_host_instance`；Kernel 侧的掉电注入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；C-COMMIT-001 |
| C-HOST-015 | M1 已实现（存储端口缺失时）：`KernelLifecycle.start_without_storage_is_unsupported_and_stays_configuring`；Native Host 在存储打开之前表中没有存储端口（ADR-0026）：`NativeHost.table_has_storage_ports_only_while_storage_is_open`；`MA-005`；其余 Host 侧随 `MA-007` |
| C-HOST-016 | M0 已实现：`KernelApi.create_rejects_missing_clock`；M1 起另有 `KernelApi.create_rejects_missing_wake_or_ingress_lock`、`KernelApi.host_table_without_0_3_ports_is_invalid_argument` |
| C-HOST-017 | M1 已实现（存储端口：模拟与 Native Host 通过同一套 `tests/unit/host_storage_contract_test.cpp`，ADR-0026）：`HostStorage.every_accepted_request_completes_once_with_its_own_attempt`、`HostStorage.kernel_boots_on_each_new_host_instance`、`HostStorage.damaged_batch_before_the_last_is_never_dropped`；其余端口的共享套件随各自的包；M2 ESP32 复核 |
| C-HOST-018 | M1 已实现（Kernel 侧）：`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`、`KernelLifecycle.append_completing_while_draining_is_still_applied`、`WorkJournal.draining_stores_every_batch_before_it_stops`；Native 存储关闭时完成全部已受理请求（ADR-0026）：`NativeHost.closing_storage_answers_every_accepted_request`、`HostStorage.every_accepted_request_completes_once_with_its_own_attempt`；`MA-003`、`MA-005`；其余 Host 侧随 `MA-007` |
| C-HOST-019 | M1 已实现（恢复路径，同一时刻只有一个存储请求）：模拟 Host 在完成时才读写缓冲，`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`；运行中的批次留在环中，追加完成才出环，之前不被覆盖（ADR-0026）：`WorkJournal.transition_waits_for_room_for_its_batch`；Native Host 的 I/O 线程在投递完成之前读写缓冲：`HostStorage.append_then_read_returns_the_same_bytes` |
| C-HOST-020 | M1 已实现（完成槽与事件）：`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionWake.result_is_applicable_when_the_owner_is_woken`、`NativeHost.ingress_lock_excludes_other_threads`、`SimHost.host_calls_under_the_ingress_lock_are_recorded`、`SimHostDeathTest.second_ingress_lock_on_one_thread_aborts`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`（恢复全程没有持锁调用）、`KernelEvents.states_between_start_and_stopped_take_posts`、`KernelEvents.posts_from_host_threads_are_each_taken_once`、`KernelEvents.posts_racing_shutdown_are_each_counted_once`；ESP-IDF Host 随 `MA-007` 复核 |
| C-HOST-021 | M1 已实现（Kernel 报告的期限）：`WorkTrigger.delay_goal_derives_once_its_time_comes`、`WorkTrigger.deadline_counts_only_goals_that_could_derive`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`、`WorkTrigger.paused_instance_holds_the_occurrence_until_resumed`、`WorkTrigger.used_up_ids_report_no_deadline`、`WorkTrigger.budget_spent_before_deriving_still_reports_the_due_occurrence`、`WorkTrigger.draining_reports_no_deadline`、`WorkExpiry.goal_expires_at_its_time`、`WorkExpiry.expiry_counts_in_the_deadline`、`WorkExpiry.expiry_needs_neither_room_nor_an_id`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.used_up_ids_skip_with_their_reason`、`WorkWait.unbounded_wait_never_skips`、`WorkWait.paused_goal_waits_without_skipping`、`WorkWait.deadline_is_the_end_of_the_earliest_bounded_wait`、`WorkExpiry.clock_at_its_largest_value_finds_no_due_work`、`WorkInput.goal_takes_inputs_from_its_source`、`WorkInput.inputs_waiting_too_long_are_skipped`；按期限睡眠的 step 循环随 E1 夹具与 `MA-007` |
| C-POLICY-001 | M1 已实现（GoalType 的校验与策略的 `decide` 期间，修改 Kernel 的调用返回 `INVALID_STATE` 且不产生效果，ADR-0022）：`WorkModel.validator_cannot_change_the_kernel_while_it_runs`、`WorkDecision.strategy_cannot_change_the_kernel_while_it_decides`；不执行 I/O、不阻塞只能评审；`MA-009` |
| C-POLICY-002 | M1 已实现（超过 `max_intents_per_call` 的意图、超过 `max_checkpoint_bytes` 的私有检查点或策略返回 `CAPACITY_EXCEEDED` 时整次决定被拒绝，Task 停下并记 `POLICY_OUTPUT_LIMIT`，不提交步骤与检查点；上限在注册时检查，ADR-0022、ADR-0024）：`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.kernel_reads_only_its_own_checkpoint_buffer`、`WorkStrategy.registration_checks_its_arguments`；`MA-009`；C-LIMIT-006 |
| C-POLICY-003 | M1 已实现（结构大小、种类、版本与互相矛盾的结束意图；`step_key`、重复的步骤与步骤上限；策略只写 Kernel 提供的意图数组与私有检查点缓冲，注册时确认它写意图的步长与 Kernel 一致，Kernel 只读回自己的缓冲，并按自己保存的上限读私有检查点，ADR-0022、ADR-0024）：`WorkDecision.refused_decisions_hold_the_task_with_their_reason`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkStrategy.registration_checks_its_arguments`、`WorkDecision.strategy_starts_from_cleared_intents_that_only_the_kernel_reads`、`WorkCheckpoint.kernel_reads_only_its_own_checkpoint_buffer`、`WorkDecision.end_that_would_exhaust_a_revision_holds_the_task`、`WorkCheckpoint.end_that_would_exhaust_a_revision_commits_nothing`；授权、预算与资源随 `MA-006`、`MA-018`；E3 步骤 2；E4 步骤 1；C-TRUST-001 |
| C-POLICY-007 | M1 已实现（私有检查点带写入它的策略 ID 与版本，下一次决定从它开始；被拒绝的决定不改变它，ADR-0024）：`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.task_in_a_given_up_place_starts_without_steps_or_checkpoint`；M1 已实现（重启后：检查点不是 GoalType 现在的策略在当前版本写的、或 GoalType 不再有策略时，Task 停下并报告 `UNSUPPORTED_VERSION`（`STRATEGY_CHANGED`），步骤、检查点与版本不变，注册了原策略的启动从它继续，ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`、`TaskRestore.restore_checkpoint_replays_the_private_checkpoint`、`TaskRestore.hold_restored_holds_a_live_task_at_its_revision`；E3 步骤 5 |
| C-POLICY-008 | M1 已实现（`REPORT_PROGRESS` 以目录 ID 形式的 `step_key` 提交步骤，在 Task 内唯一，只增不删，快照按提交顺序给出，ADR-0024）：`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`、`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`、`WorkCheckpoint.steps_bound_how_often_a_task_is_decided`、`WorkCheckpoint.steps_that_share_a_prefix_are_distinct`、`TaskTable.decisions_commit_steps_checkpoints_and_progress`、`TaskTable.add_step_refuses_a_taken_invalid_or_extra_step`；M1 部分实现（跨重启：步骤与 `step_key` 由 Kernel 保存并按提交顺序恢复，换版本后的决定看到同样的步骤，ADR-0026）：`TaskRestore.restore_step_replays_each_step_at_its_position`、`WorkRecovery.task_its_strategy_cannot_serve_is_held`；换策略不重做随 `MA-009`，调用的步骤随 `MA-008`；E3 步骤 4；E4 步骤 2；C-ID-003 |
| C-POLICY-009 | M1 已实现（未知的类型或版本拒绝，参数由 GoalType 校验；GoalType 声明它接受的触发种类，Goal 只能选择其中之一，M1 为 `NONE`、`ONCE`、`DELAY`、`INTERVAL`、`LOCAL_INPUT`，ADR-0018、ADR-0019、ADR-0021；GoalType 声明 occurrence 等待 Task 槽位或 ID 的上限，ADR-0020；会产生 Task 的 GoalType 指定决定其 Task 的策略，完成由策略的决定表达，ADR-0022）：`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`、`GoalTypeRegistry.tells_an_unknown_id_from_an_unknown_version`、`GoalTypeRegistry.validator_decides_the_parameters`、`GoalTypeRegistry.type_without_validator_takes_no_parameters`、`GoalTypeRegistry.trigger_kinds_decide_the_accepted_triggers`、`WorkTask.goal_type_must_accept_the_trigger`、`WorkInput.goal_type_must_accept_local_input`、`WorkModel.registration_checks_its_arguments`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.zero_wait_skips_at_once`、`WorkWait.unbounded_wait_never_skips`、`WorkStrategy.goal_type_names_a_strategy_exactly_when_its_goals_may_have_tasks`、`WorkDecision.goal_ends_with_the_last_of_its_tasks`；观察与订阅事件的触发随 `MA-018`，日历规则随 `MA-011`，暂停点与 Task 执行中的等待上限随 `MA-008`、`MA-018`；E4 步骤 5 |
| C-POLICY-010 | M1 已实现（策略只在 `CONFIGURING` 中注册，同一 ID 同时只有一个版本，ADR-0022）：`WorkStrategy.registration_checks_its_arguments`、`StrategyRegistry.refuses_another_version_of_an_id_and_registrations_beyond_capacity`、`WorkModel.return_values_follow_the_state_table`；M1 已实现（重启后换版本：只有写入检查点的策略在同一版本读取它，否则 Task 停下，ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`；E3 步骤 2、4 |
| C-STORE-001 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_accepted`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-002 | M1 已实现：Kernel 侧 `BatchCodec.every_short_write_is_corrupt`、`PrefixScan.damaged_batch_stops_the_scan`；Host 丢弃撕裂的尾部 `HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.torn_tail_is_truncated_before_the_next_append`；追加中断电的批次只在完整写入时计入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；重放受限时丢弃已重放的全部内容 `WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`（ADR-0026）；E2 F0、F7、F8 |
| C-STORE-003 | M1 已实现：`PrefixScan.commit_seq_must_continue_exactly`、`PrefixScan.older_writer_after_a_newer_one_is_corrupt`（`WRITER_FENCE` 之前 `writer_epoch` 回退也是损坏）、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；运行中每个 transition 取下一个 `commit_seq` 并写一个批次，不改变持久状态的写空批次，追加失败后不再写入，所以不留缺口：`WorkJournal.append_that_fails_restricts_the_kernel`、`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`（ADR-0026）；快照之后的起点随 `MA-010` 的 F 部分 |
| C-STORE-004 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_follow_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_are_skipped`；E4 步骤 6；C-VER-020 |
| C-STORE-009 | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；M1 已实现（工作记录违反表规则或日志末尾不一致时为 `LOG_CORRUPT`，已重放的内容全部丢弃，ADR-0026 决策 5）：`WorkRecovery.log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt`、`WorkReplay.goal_created_without_its_state_is_corrupt`、`WorkReplay.live_task_under_an_ended_goal_is_inconsistent`、`GoalRestore.restore_state_refuses_a_change_to_a_terminal_goal_or_an_impossible_cursor`、`TaskRestore.restore_state_refuses_a_change_to_an_ended_or_held_task`、`AgentRestore.record_no_transition_writes_is_corrupt`；E2 F7（已提交区域损坏） |
| C-STORE-013 | M1 已实现（工作模型：实例、Goal、Task、步骤、私有检查点与命令映射只写入日志，每个 transition 一个批次，ADR-0026）：`WorkRecovery.power_loss_takes_the_batches_never_appended`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；预算等其余状态随各自的包 |
| C-STORE-014 | M1 已实现（工作记录：Goal 只写时长，Task 不写 `due_us`，恢复时从恢复结束起重新计时，ADR-0026）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`；评审 |
