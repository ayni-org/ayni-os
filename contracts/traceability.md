# 契约追踪矩阵

本文件由 `tools/check_contracts.py --write` 根据各契约文档生成，请勿手工编辑。修改条款后重新生成；本文件过期时 CTest 用例 `check_contracts_repository` 失败。

## 概览

| 文档 | 条款数 | 已实现 |
|---|---:|---:|
| [domain/authority_and_trust.md](domain/authority_and_trust.md) | 21 | 1 |
| [domain/calls_and_bindings.md](domain/calls_and_bindings.md) | 19 | 0 |
| [domain/commit_and_output.md](domain/commit_and_output.md) | 19 | 3 |
| [domain/ids_and_time.md](domain/ids_and_time.md) | 18 | 5 |
| [domain/lifecycles.md](domain/lifecycles.md) | 17 | 9 |
| [domain/limits.md](domain/limits.md) | 12 | 6 |
| [domain/permits_and_stop.md](domain/permits_and_stop.md) | 27 | 0 |
| [domain/primitives.md](domain/primitives.md) | 8 | 0 |
| [domain/product_semantics.md](domain/product_semantics.md) | 28 | 0 |
| [domain/resources_and_budget.md](domain/resources_and_budget.md) | 23 | 0 |
| [domain/status_codes.md](domain/status_codes.md) | 7 | 3 |
| [domain/versions.md](domain/versions.md) | 17 | 4 |
| [host/host_ports.md](host/host_ports.md) | 20 | 10 |
| [host/policy_interface.md](host/policy_interface.md) | 11 | 1 |
| [host/provider.md](host/provider.md) | 14 | 0 |
| [storage/records.md](storage/records.md) | 14 | 4 |
| [wire/envelope.md](wire/envelope.md) | 16 | 0 |
| [wire/json_subset.md](wire/json_subset.md) | 12 | 0 |
| 合计 | 303 | 46 |

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

## 工作包 → 条款

| 工作包 | 条款 |
|---|---|
| `MA-001` | C-STOP-003、C-STOP-004 |
| `MA-003` | C-TIME-001、C-LIMIT-001、C-LIMIT-008、C-LIMIT-011、C-STOP-001、C-HOST-002、C-HOST-003、C-HOST-006、C-HOST-018 |
| `MA-004` | C-AUTH-004、C-COMMIT-012、C-LIFE-003、C-LIFE-020、C-LIFE-021、C-LIFE-022、C-LIFE-025、C-LIMIT-007、C-HOST-020、C-POLICY-009、C-WIRE-014 |
| `MA-005` | C-LIFE-002、C-LIFE-005、C-LIFE-006、C-LIFE-007、C-LIMIT-002、C-STATUS-001、C-STATUS-003、C-STATUS-006、C-HOST-015、C-HOST-018、C-PROV-001、C-WIRE-014、C-JSON-009、C-JSON-012 |
| `MA-006` | C-AUTH-001、C-AUTH-003、C-AUTH-004、C-AUTH-008、C-AUTH-010 |
| `MA-007` | C-HOST-002、C-HOST-007、C-HOST-008、C-HOST-015、C-HOST-018、C-HOST-020、C-WIRE-001、C-WIRE-002、C-WIRE-003、C-WIRE-013、C-WIRE-014、C-JSON-001、C-JSON-002、C-JSON-003、C-JSON-004、C-JSON-009 |
| `MA-008` | C-CALL-001、C-CALL-003、C-BIND-002、C-BIND-003、C-LIFE-003、C-BUDGET-005、C-PROV-002、C-PROV-003、C-PROV-010 |
| `MA-009` | C-LIMIT-005、C-LIMIT-006、C-POLICY-001、C-POLICY-002、C-POLICY-005、C-POLICY-011 |
| `MA-010` | C-COMMIT-001、C-COMMIT-003、C-COMMIT-004、C-COMMIT-014、C-COMMIT-016、C-ID-001、C-ID-006、C-ID-009、C-ID-010、C-TIME-003、C-LIFE-025、C-VER-020、C-VER-023、C-HOST-009、C-HOST-010、C-HOST-011、C-HOST-019、C-STORE-001、C-STORE-002、C-STORE-003、C-STORE-006、C-STORE-007、C-STORE-013、C-STORE-014 |
| `MA-011` | C-TIME-004、C-TIME-005、C-TIME-006、C-TIME-007、C-LIMIT-012、C-PROD-018、C-PROD-032、C-PROD-034、C-PROD-042、C-JSON-010 |
| `MA-012` | C-LIMIT-010 |
| `MA-015` | C-PRIM-005、C-PROV-013、C-PROV-014、C-WIRE-011 |
| `MA-016` | C-AUTH-002、C-AUTH-012、C-AUTH-015、C-LIFE-020、C-PRIM-007、C-VER-010、C-WIRE-003、C-WIRE-006、C-WIRE-007、C-WIRE-009、C-WIRE-010 |
| `MA-018` | C-AUTH-016、C-LIMIT-002、C-PERMIT-001、C-PERMIT-003、C-PERMIT-016、C-PRIM-001、C-PRIM-008、C-RES-001、C-RES-003、C-RES-006、C-RES-012、C-RES-013 |
| `MA-019` | C-PROD-012、C-PROD-015、C-PROD-018、C-PROD-050 |
| `MA-026` | C-AUTH-011、C-PERMIT-010、C-PERMIT-011、C-PERMIT-012、C-PERMIT-016、C-PERMIT-017、C-STOP-002、C-STOP-003、C-STOP-004、C-STOP-005、C-STOP-007、C-STOP-008、C-STOP-009、C-STOP-010、C-STORE-012 |
| `MA-027` | C-PROD-033、C-BUDGET-010、C-PROV-011 |
| `MA-028` | C-PROD-001、C-PROD-003、C-PROD-004、C-PROD-051 |
| `MA-030` | C-WIRE-015 |
| `MA-034` | C-TRUST-002 |
| `MA-035` | C-PERMIT-014、C-JSON-011 |
| `MA-037` | C-TRUST-005 |
| `MA-038` | C-AUTH-012、C-AUTH-013、C-ID-005、C-TIME-002、C-PERMIT-001、C-PERMIT-015、C-PERMIT-017、C-RES-012、C-WIRE-006、C-WIRE-008、C-WIRE-009、C-WIRE-014、C-JSON-010 |

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
| C-COMMIT-004 | M1 已实现（查询标注：对象带 `commit_seq`，`ayni_get_kernel_info` 报告 `durable_seq`，结果未知的追加不计入）：`KernelLifecycle.durable_seq_follows_the_log_read_and_then_the_boot_batch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`WorkModel.created_goal_is_applied_at_once_and_not_yet_durable`；输出的 `requires_seq` 门控随 `MA-010`；E2 F6、F9；属性测试：每个已释放输出的 seq ≤ 恢复后的 `durable_seq` |
| C-COMMIT-012 | M1 已实现（Goal 意图：创建取 0，其余取目标的当前版本）：`WorkModel.stale_revision_is_a_conflict_and_changes_nothing`、`GoalTable.stale_revision_is_a_conflict_before_the_state_is_checked`、`GoalTable.change_past_the_largest_revision_is_refused`、`WorkModel.change_past_the_largest_revision_is_refused_and_changes_nothing`；策略意图及其依赖版本随 `MA-004` 第三部分；E1 步骤 9；E4 步骤 4 |
| C-COMMIT-016 | M1 已实现：`WorkModel.commit_seq_of_unstored_work_is_reused_by_later_boots`；输出的 `requires_seq` 门控随 `MA-010` |
| C-ID-001 | M1 已实现（Kernel 分配的 ID 不复用；对象尚未持久化，重启后不恢复）：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`；删除与 tombstone 随 `MA-010`；E1 步骤 9 |
| C-ID-006 | M1 已实现（完成槽）：`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`；恢复请求以 `boot_epoch` 0 命名且 `request_id` 依次递增：`KernelLifecycle.recovery_requests_use_boot_epoch_0`；Host 不投递旧实例结果的契约测试随 `MA-010`；E2 F7；E3 步骤 2 |
| C-ID-009 | M1 已实现（启动）：`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`、`KernelLifecycle.each_reboot_persists_the_next_epoch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.reboot_after_an_unknown_append_skips_its_epoch`；受限栅栏重载随 `MA-010`；E2 F12 |
| C-ID-010 | M1 已实现：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`、`KernelLifecycle.largest_epoch_and_transition_id_still_boot`；实例跨重启保持同一 `instance_id` 随 `MA-010` |
| C-TIME-008 | M0 已实现：`NativeHost.monotonic_clock_never_decreases`（Native）；ESP32 使用 `esp_timer_get_time`，M2 补测 |
| C-LIFE-001 | M0 已实现：`KernelApi.create_in_caller_memory_starts_configuring` |
| C-LIFE-002 | M1 已实现：`KernelApi.kernel_memory_is_reusable_after_destroy`、`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`；`MA-005` |
| C-LIFE-003 | M1 已实现（注册只在 `CONFIGURING`）：`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.return_values_follow_the_state_table`；交叉引用校验随引用其他目录项的注册（策略：`MA-004` 第三部分；绑定：`MA-008`） |
| C-LIFE-004 | M1 已实现：`WorkModel.intent_outside_running_is_not_ready`、`WorkModel.draining_answers_queries_and_closes_sessions_but_takes_no_work`、`WorkModel.return_values_follow_the_state_table`；E2 F13 |
| C-LIFE-007 | M1 已实现：`KernelLifecycle.return_values_follow_the_state_table`、`WorkModel.return_values_follow_the_state_table`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.restriction_is_reported_only_while_restricted`；`MA-005` |
| C-LIFE-020 | M1 已实现（Goal：创建时指定本 Kernel 的实例，之后没有改变 owner 的意图）：`WorkModel.owner_must_be_an_instance_of_this_kernel`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.child_must_have_its_parents_owner`；Task 随 `MA-004` 第二部分；`MA-016` |
| C-LIFE-021 | M1 已实现（Goal 状态与版本）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.pause_and_resume_change_only_the_goal`、`GoalTable.lifecycle_decides_which_changes_are_allowed`；停止派生 Task 与在途 Task 的暂停点随 `MA-004` 第二部分传播矩阵 |
| C-LIFE-022 | M1 已实现（Goal 及其后代在一个 transition 中取消并记录 `cancel_revision`）：`WorkModel.pause_stays_on_the_goal_and_cancel_reaches_its_descendants`、`GoalTable.cancel_reaches_every_live_descendant_and_nothing_else`；传播到 Task 与在途请求随 `MA-004` 第二、三部分；E4 步骤 3 |
| C-LIFE-025 | M1 已实现（实例尚未持久化，每次启动重新创建）：`WorkModel.instances_start_with_the_kernel_in_one_transition`、`AgentTable.start_all_gives_each_instance_an_id_and_the_stamp`、`AgentTable.registers_each_agent_once_up_to_capacity`；跨重启保持实例随 `MA-010`；策略初始化与 `STARTING` 随 `MA-004` 第三部分 |
| C-LIMIT-001 | M1 已实现（入口、完成槽、存储缓冲与工作模型）：`KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`、`IngressQueue.ten_thousand_events_pass_in_order_without_loss`、`KernelApi.capacity_outside_its_range_is_invalid_argument`、`KernelApi.memory_requirements_grow_with_each_capacity`、`KernelApi.goal_tree_limits_need_no_memory`、`WorkModel.full_goal_table_refuses_without_side_effects`；其余容量随各自工作包；`MA-003`；第 10.6 节 |
| C-LIMIT-002 | M1 已实现（已有的限额）：入口 `IngressQueue.full_queue_rejects_other_events_with_busy`；完成槽 `CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`；存储缓冲 `KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`；主体、Agent 与 GoalType `WorkModel.registration_refuses_duplicates_and_capacity`；Goal `WorkModel.full_goal_table_refuses_without_side_effects`；Goal 参数 `WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`；子目标 `WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；会话 `WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`；ID `WorkModel.used_up_ids_refuse_goals_and_sessions`；其余限额随各自工作包；`MA-018`；`MA-005` |
| C-LIMIT-004 | M0 已实现：ctest `core_does_not_reference_heap_allocator` 检查每个 Core 静态库不引用 `malloc`/`free`/`operator new`/`operator delete`，也不引用 `strdup`、`asprintf`、`getline` 等替调用方分配内存的 libc 函数；ctest `heap_check_detects_allocator` 与 ctest `heap_check_detects_strdup` 证明该检查有效 |
| C-LIMIT-007 | M1 已实现（子目标深度与数量）：`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.tree_limits_refuse_deeper_or_more_children`、`GoalTable.zero_limits_allow_top_level_goals_only`、`WorkModel.tree_limits_missing_from_the_config_take_their_defaults`；周期触发随 `MA-004` 第二部分；E2 F8 |
| C-LIMIT-008 | M1 已实现（入口与完成槽）：`IngressQueue.full_queue_rejects_other_events_with_busy`、`IngressQueue.full_queue_replaces_newest_value_of_the_same_stream`、`IngressQueue.telemetry_flood_keeps_each_stream_in_order_and_accounted`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；E1 步骤 10；`MA-003` |
| C-LIMIT-011 | M1 已实现：`KernelLifecycle.step_applies_at_most_max_items`、`KernelLifecycle.step_returns_once_its_time_budget_is_spent`、`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`（请求在之后的 step 中完成）、`KernelLifecycle.recovery_waits_for_a_free_slot_without_spinning`（等待空闲槽时不报告 `more_ready`）；`MA-003`；第 10.6 节「事件与容量」 |
| C-STATUS-001 | M0 已实现：`include/ayni/ayni.h` 为每个已实现函数列出返回码，`tests/unit/kernel_api_test.cpp` 覆盖 `ayni_create` 文档列出的每个失败码；`MA-005` 生命周期返回值表：`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.step_rejects_bad_arguments_without_effect`、`KernelLifecycle.kernel_info_checks_its_output`；工作模型：`WorkModel.return_values_follow_the_state_table`、`WorkModel.intent_arguments_are_refused_before_anything_else`、`WorkModel.registration_checks_its_arguments`、`WorkModel.queries_check_their_arguments` |
| C-STATUS-002 | M0 已实现：`KernelApi.failed_create_leaves_memory_untouched` |
| C-STATUS-006 | M1 已实现（`ayni_submit_intent`：每次返回都写状态、原因码与对象引用，只含 ID 与版本）：`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written`、`WorkModel.intent_through_a_closed_or_unknown_session_is_denied`、`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；日志与诊断输出的测试随其实现（M1）；`MA-005` |
| C-VER-001 | M0 已实现：`KernelApi.create_rejects_other_abi_major`、`KernelApi.create_rejects_other_abi_minor`、`KernelApi.config_from_abi_0_1_is_unsupported_version`、`KernelApi.failed_create_leaves_memory_untouched` |
| C-VER-002 | M1 已实现：编译期检查 `tests/contract/abi_layout.c` 以 `offsetof` 与 `sizeof` 固定已发布结构体的布局；评审 |
| C-VER-003 | M0 已实现：`KernelApi.create_copies_host_table_before_return`；`KernelApi.config_from_abi_0_1_is_unsupported_version`（`struct_size` 不含 `abi_minor` 时按 0.1 处理）；M1 起另有 `KernelApi.host_table_without_0_3_ports_is_invalid_argument`、`KernelApi.host_table_without_storage_ports_is_accepted`、`KernelApi.uncovered_capacities_take_their_defaults`、`KernelLifecycle.kernel_info_checks_its_output`；0.5 新增的结构体只有一个布局，只接受自身的 `sizeof`：`WorkModel.registration_checks_its_arguments`、`WorkModel.queries_check_their_arguments`、`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written` |
| C-VER-004 | M0 已实现：`KernelApi.struct_size_below_first_layout_is_invalid_argument`、`KernelApi.struct_size_from_newer_minor_is_unsupported_version` |
| C-HOST-001 | M0 已实现：`KernelApi.create_copies_host_table_before_return` |
| C-HOST-004 | M1 已实现（Kernel 侧）：`CompletionPoolTest.rejected_request_frees_the_slot_and_runs_no_handler`、`CompletionPoolTest.accept_unknown_waits_for_the_post`、`CompletionPoolTest.undefined_submit_result_counts_as_in_flight`、`CompletionPoolTest.post_before_a_rejection_is_still_applied`；E2 F10；C-OUT-003 |
| C-HOST-006 | M1 已实现（完成槽部分）：`CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`、`CompletionPoolTest.result_is_applied_once`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`、`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionPoolTest.concurrent_posts_apply_each_result_exactly_once`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；遥测合并见 C-LIMIT-008，订阅的 gap 报告随订阅实现；E1 步骤 10；`MA-003` |
| C-HOST-007 | M1 已实现：`NativeHost.wakes_from_other_threads_merge_into_one`、`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`；ESP-IDF Host 随 `MA-007` 复核 |
| C-HOST-009 | M1 已实现（`append` 与 `read_prefix` 的 Kernel 侧）：`KernelApi.create_rejects_only_one_storage_port`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`；Host 实现与其余端口随 `MA-010`；E2 F7、F8 |
| C-HOST-015 | M1 已实现（存储端口缺失时）：`KernelLifecycle.start_without_storage_is_unsupported_and_stays_configuring`；`MA-005`；Host 侧随 `MA-007` |
| C-HOST-016 | M0 已实现：`KernelApi.create_rejects_missing_clock`；M1 起另有 `KernelApi.create_rejects_missing_wake_or_ingress_lock`、`KernelApi.host_table_without_0_3_ports_is_invalid_argument` |
| C-HOST-018 | M1 已实现（Kernel 侧）：`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`、`KernelLifecycle.append_completing_while_draining_is_still_applied`；`MA-003`、`MA-005`；Host 侧随 `MA-007` |
| C-HOST-019 | M1 已实现（恢复路径，同一时刻只有一个存储请求）：模拟 Host 在完成时才读写缓冲，`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`；多个在途批次随 `MA-010`；Host 契约测试套件 |
| C-HOST-020 | M1 已实现（完成槽部分）：`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionWake.result_is_applicable_when_the_owner_is_woken`、`NativeHost.ingress_lock_excludes_other_threads`、`SimHost.host_calls_under_the_ingress_lock_are_recorded`、`SimHostDeathTest.second_ingress_lock_on_one_thread_aborts`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`（恢复全程没有持锁调用）；事件部分随 `MA-004` 的 `ayni_post_event`；ESP-IDF Host 随 `MA-007` 复核 |
| C-POLICY-009 | M1 已实现（未知的类型或版本拒绝，参数由 GoalType 校验）：`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`、`GoalTypeRegistry.tells_an_unknown_id_from_an_unknown_version`、`GoalTypeRegistry.validator_decides_the_parameters`、`GoalTypeRegistry.type_without_validator_takes_no_parameters`；触发、完成条件、暂停点与等待上限随 `MA-004` 第二、三部分；E4 步骤 5 |
| C-STORE-001 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_accepted`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-003 | M1 已实现：`PrefixScan.commit_seq_must_continue_exactly`、`PrefixScan.older_writer_after_a_newer_one_is_corrupt`（`WRITER_FENCE` 之前 `writer_epoch` 回退也是损坏）、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；快照之后的起点随 `MA-010` |
| C-STORE-004 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_follow_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_are_skipped`；E4 步骤 6；C-VER-020 |
| C-STORE-009 | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；E2 F7（已提交区域损坏） |
