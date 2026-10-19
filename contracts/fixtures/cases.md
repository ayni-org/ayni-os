# E1–E4 用例清单

M1 要实现的确定性用例，格式见 [README.md](README.md)。每个计划步骤至少有一个用例；同一场景只在切断点或回答上不同的，在一行中列出多个用例 ID，每个 ID 仍是一个独立文件。条款列只写该用例主要证明的条款，步骤与条款的完整对应见 [traceability.md](../traceability.md)。本清单在 M1 写用例时可以增补，删除或改名须说明原因。

E1 的确定性基线沿用计划 §10.2：每实例模型额度 2 次、设备总额度 4 次；正常 Mock 在虚拟 100 ms 完成，超时用例延迟到 1,500 ms，请求 deadline 1,000 ms；资源等待上限 500 ms；本地规则事件在 10 次 `step` 内处理。

## E1：多个 Agent 共用系统服务

| 用例 ID | 场景与注入 | 主要断言 | 条款 |
|---|---|---|---|
| `E1.S1.private_read_denied` | A、B 各写一条私有事实，再读取对方记录 | 读取被拒，结果与轨迹中没有对方正文 | C-PRIM-007、C-PROD-042、C-WIRE-010 |
| `E1.S1.explicit_share_then_revoke` | A 显式共享一条给 B，B 读取；撤销后 B 再组装上下文 | 共享期间可读，撤销后不进入新上下文；访问记录关联主体与任务 | C-PROD-041、C-WIRE-010 |
| `E1.S2.model_slot_contention` | B 占用模型槽且 Mock 延迟，A 请求同一槽 | A 的 Task `blocked_on` 含模型槽；不变式：模型活跃数 ≤ 1 | C-RES-009、C-PROV-012 |
| `E1.S2.local_rule_progresses` | 同上状态下向 A 注入本地按键事件 | 本地规则工作在 10 次 `step` 内完成，`step` 不等待 Provider | C-RES-011 |
| `E1.S3.exclusive_queue_shared_parallel` | A、B 竞争麦克风，同时打开共享摄像头订阅 | 麦克风排队、摄像头并行；轨迹记录准入顺序与最长等待 | C-RES-004、C-RES-009、C-RES-010 |
| `E1.S3.resource_group_all_or_nothing` | 需要麦克风与模拟执行器两项资源的调用，其中一项已被占用 | 两项都不占用；释放后整组准入；无死锁 | C-RES-001、C-RES-002、C-RES-006 |
| `E1.S4.timeout_cancel_releases` | Mock 延迟到 1,500 ms，超过 1,000 ms deadline，再取消等待任务 | 取消结果按 Provider 报告区分；资源与终态在同一 transition 释放 | C-CALL-009、C-RES-007、C-HOST-005、C-PROV-006 |
| `E1.S4.unconfirmed_stop_quarantines` | 模拟执行器只报告停止指令已送达，没有停止确认 | 资源进入 `QUARANTINED`，不分配给下一动作；三种停止事实分别记录 | C-RES-008、C-STOP-006、C-LIFE-012、C-PRIM-004 |
| `E1.S5.instance_quota_exhausted` | A 用完 2 次模型额度后再请求；B 发起合法工作 | A 被拒并暂停、报告预算原因；B 在设备总额 4 次内继续 | C-BUDGET-001、C-BUDGET-007、C-LIMIT-005、C-RES-003 |
| `E1.S5.restart_keeps_consumption` | 额度耗尽后 `restart` | 已花额度不清零，A 仍被拒 | C-BUDGET-003 |
| `E1.S6.revoke_before_dispatch` | B 的运动请求排队时撤销运动授权 | 派发前拒绝；`trace_absent`：撤销之后的派发 | C-AUTH-005、C-AUTH-011 |
| `E1.S6.revoke_in_flight` | 动作在途时撤销 | 发出取消；撤销响应依次为 `ACCEPTED`、`CANCELLING`、`STOP_CONFIRMED`；不显示为从未执行 | C-LIFE-013、C-AUTH-005 |
| `E1.S6.unauthorised_motion_rejected` | A 提交 `motion.*` 调用 | 始终拒绝，派生工作不扩大授权 | C-AUTH-001、C-AUTH-002、C-AUTH-004 |
| `E1.S7.profile_scoped_to_agent` | 公共档案只授权 A，B 读取 | B 被拒；A 的私有记忆仍不对 B 可见 | C-PROD-040 |
| `E1.S7.voice_cannot_grant` | 按键语音意图尝试新增 Grant 与敏感删除，再由已绑定管理端提交 | 语音被拒且不生成待批授权；管理端命令成功并记录主体与渠道 | C-AUTH-009、C-AUTH-010、C-PROD-002、C-PROD-035、C-TRUST-001 |
| `E1.S8.close_stops_generation` | 观察订阅 `ACTIVE` 后关闭，再投递旧代际尾部事件；之后重开 | 等待停止确认；旧代际事件不进入上下文；重开为新代际 | C-SUB-001、C-SUB-002、C-PROD-023 |
| `E1.S8.renewal_rechecks_grant` | 撤销观察授权后到达续租时刻 | 续租失败，立即停止新采集与上传；资源按规则释放或隔离；媒体引用回收 | C-SUB-003、C-RES-010、C-PRIM-005 |
| `E1.S9.delete_blocks_stale_summary` | 模型整理读取记录 X 后删除 X，再返回基于 X 的摘要 | 写回被 tombstone 拒绝，派生摘要失效；费用只结算一次 | C-PROD-030、C-PROD-031、C-BUDGET-009、C-PROV-007 |
| `E1.S9.user_edit_not_overwritten` | 整理期间用户修改 X，旧结果返回 | 返回 `AYNI_REVISION_CONFLICT`，用户的新版本保留 | C-COMMIT-012、C-ID-001 |
| `E1.S9.cleanup_resumes_after_restart` | 删除后的正文清理进行中 `restart` | 清理继续，被删内容不复活 | C-PROD-030 |
| `E1.S10.completion_slots_when_queue_full` | 普通事件队列满时投递存储确认与终态，并继续 submit | 确认与终态不丢失；完成槽耗尽时新 submit 返回 `AYNI_BUSY`；重复完成只应用一次 | C-HOST-006、C-LIMIT-008、C-CALL-012 |
| `E1.S10.ordinary_work_expires_with_reason` | 持续的高优先工作压住普通工作 | 普通工作在等待上限后延后或过期并带原因码；保留额度不被占用 | C-RES-004、C-RES-005、C-BUDGET-006 |
| `E1.S10.stop_bypasses_full_queue` | 队列满时触发急停 | 停止锁存立即生效，不等待队列空位 | C-STOP-001 |
| `E1.S11.pause_keeps_reminder` | 暂停助理时已有提醒到期 | 自主观察与运动收敛，提醒照常呈现，界面状态为“提醒继续” | C-PROD-020、C-PROD-021、C-LIFE-023 |
| `E1.S11.presentation_success_completes` | 呈现器返回成功 | 只有成功证据使 occurrence 完成；入列与发送成功不算 | C-PROD-010、C-CALL-003 |
| `E1.S11.presentation_failed_pending`、`E1.S11.presentation_unknown_pending` | 呈现器返回失败；返回未知 | 提醒保持待处理，分别显示 `PRESENTATION_FAILED`、`PRESENTATION_UNKNOWN` | C-PROD-013 |
| `E1.S11.duplicate_receipt_once`、`E1.S11.receipt_lost_reconciled` | 回执重复；回执丢失后重启 | 重复回执不重复完成；丢失时按渠道属性对账，已确认的不重播 | C-PROD-014、C-PROD-013 |
| `E1.S11.quiet_hours_deferred` | 提醒在安静时段到期 | 保持 `AVAILABLE`，按策略延后或过期，不标为完成；同一 occurrence 只触发一次 | C-PROD-016、C-TIME-005 |
| `E1.S11.estop_keeps_reminder` | 提醒待呈现时按下急停 | 运动准入立即禁止；提醒不被取消，目标不被删除 | C-STOP-002 |

## E2：动作关键阶段重启

所有 E2 用例使用不可默认重复的 `test.increment` 能力，断言 Host 侧副作用计数。凡涉及结果未知的注入点，都分别有“支持权威查询”和“不支持查询”的用例（计划 §10.3）。

| 用例 ID | 场景与注入 | 主要断言 | 条款 |
|---|---|---|---|
| `E2.F0.prepare_torn` | 准备记录批次写入中断，未提交 | 计数 0；恢复后没有已接受动作，原因可查 | C-STORE-002 |
| `E2.F1.prepared_still_valid` | `PREPARED` 已提交、屏障未提交时重启，授权仍有效 | 重新校验后发起新尝试，最终计数 1 | C-AUTH-005、C-PRIM-001 |
| `E2.F1.prepared_grant_revoked`、`E2.F1.prepared_observation_expired` | 同上，重启前撤销授权；或前置观察过期 | 拒绝或暂停，计数始终 0；时限授权无法证明仍有效时不自动启用 | C-AUTH-005、C-AUTH-007、C-PRIM-006、C-TIME-003 |
| `E2.F2.barrier_write_failed` | `DISPATCHING` 写入 `FAILED` | Host 没有派发，计数 0，不签发许可 | C-COMMIT-006、C-PERMIT-002 |
| `E2.F2.barrier_unknown_then_found` | 写入结果 `UNKNOWN`，重启时读到完整屏障 | 保守转为 `RESULT_UNKNOWN`，不当作确定未派发 | C-COMMIT-006、C-OUT-002 |
| `E2.F3.query_not_executed` | 屏障已持久、Host 尚未发出时重启；查询回答权威未执行 | 经查询离开 `RESULT_UNKNOWN` 后重新评估，最终计数 1 | C-OUT-002、C-LIFE-010 |
| `E2.F3.query_record_not_found` | 同上，查询只回答“未找到记录” | 保持 `RESULT_UNKNOWN`，不重发，计数 0 | C-LIFE-010、C-STATUS-004 |
| `E2.F3.no_query_capability` | 同上，能力不支持查询 | 暂停并报告“结果待确认”，计数 0 | C-STATUS-004 |
| `E2.F4.in_flight_query_found`、`E2.F4.in_flight_no_query` | Host 已发出、无结果时重启；支持查询并回答已执行，或不支持查询 | 前者补终态、计数 1；后者保持未知，无自动重复 | C-LIFE-010、C-STATUS-004、C-PRIM-001 |
| `E2.F5.result_lost_query`、`E2.F5.result_lost_no_query` | 副作用已完成，结果事件丢失或完成记录写入失败后重启 | 计数保持 1；不支持查询时保持待确认，不换 operation 重做；费用保守结算 | C-ID-004、C-LIFE-010、C-BUDGET-003、C-STATUS-004 |
| `E2.F6.terminal_durable_reply_lost` | 终态已持久、回复未发出时重启，再以同一 operation 重试 | 返回已有结果，计数 1；回复在 `durable_seq` 覆盖后才释放 | C-COMMIT-004、C-WIRE-007 |
| `E2.F7.cancel_result_race` | 取消与结果同时到达 | 只终结、释放、结算一次 | C-CALL-002、C-CALL-011、C-ID-006 |
| `E2.F7.tail_short_write`、`E2.F7.tail_crc_error` | 日志尾部短写；尾部 CRC 错误 | 忽略不完整尾部，按持久前缀重建 | C-COMMIT-002、C-STORE-001、C-STORE-002 |
| `E2.F7.committed_region_corrupt` | 已提交区域损坏 | 进入 `RESTRICTED`，不跳过损坏批次 | C-STORE-009 |
| `E2.F7.old_boot_late_result` | 重启后到达旧 `boot_epoch` 的结果 | 不直接改状态，只经权威查询对账；无法验证的完成不回退为可派发 | C-CALL-011、C-ID-006、C-HOST-003 |
| `E2.F8.cut_after_cursor`、`E2.F8.cut_after_task`、`E2.F8.cut_after_reservation`、`E2.F8.cut_before_marker` | 在 occurrence 游标、Task 创建、Budget 预留之间及提交标记之前断电 | 批次全有或全无；同一 occurrence 最多一个 Task 与一份预留 | C-COMMIT-002、C-COMMIT-005、C-BUDGET-002、C-LIMIT-007 |
| `E2.F9.retry_same_command` | 创建提醒已持久接受，响应丢失后在新会话中以同一 `command_id` 重试 | 返回同一 Goal，Goal 计数 1 | C-COMMIT-004、C-WIRE-004、C-WIRE-016、C-ID-007 |
| `E2.F9.same_id_different_body`、`E2.F9.expired_command` | 同 `command_id` 不同内容；超出保留期的旧命令 | 前者冲突拒绝；后者不被当作新命令执行 | C-WIRE-004、C-WIRE-005 |
| `E2.F10.revoke_during_flush`、`E2.F10.observation_expires_during_flush`、`E2.F10.estop_during_flush` | 屏障 flush 等待中撤销、观察过期或急停 | 屏障后复核阻止 submit 并记录取消；无撤销栅栏之后的新准入 | C-COMMIT-007、C-AUTH-006、C-PRIM-006 |
| `E2.F10.revoke_after_host_accepted` | Host 队列已受理后撤销 | 视为在途：发取消并等待确认，撤销响应如实分级 | C-OUT-003、C-HOST-004、C-LIFE-013 |
| `E2.F11.cut_inside_result_batch` | 结果应用、步骤进度、摘要与结算批次中断 | 按原 transition 恢复，进度、记忆、预算闭合 | C-COMMIT-005、C-BUDGET-002、C-STORE-007 |
| `E2.F11.duplicate_result` | 重投相同结果 | 只应用一次，不双写摘要、不双重结算 | C-CALL-012 |
| `E2.F12.write_unknown_then_ok` | 写入 `UNKNOWN` 后以同一 transition 重试成功 | 只应用一次 | C-COMMIT-008 |
| `E2.F12.write_failed_fence` | 写入 `FAILED` 时有动作在途 | 受限栅栏：无新派发与许可续租，模拟执行端在失效时刻 + `L` 内停止；在途动作为 `RESULT_UNKNOWN` | C-COMMIT-009、C-PERMIT-009 |
| `E2.F12.late_old_write_rejected` | 旧 append/flush 延迟到重载之后才完成 | 被拒绝，或在下次恢复中因 `WRITER_FENCE` 被忽略；重载前 `boot_epoch` 已递增 | C-COMMIT-010、C-HOST-012、C-STORE-005、C-ID-009 |
| `E2.F12.fence_unavailable_readonly` | 旧写入方既无法排空也无法隔离 | 保持只读诊断，不重载、不恢复输出 | C-COMMIT-011 |
| `E2.F13.retry_during_recovery` | 重启后、恢复完成前重试同一 `command_id` | 恢复期间返回 `AYNI_NOT_READY`，进入 `RUNNING` 后返回原对象 | C-LIFE-004 |
| `E2.F14.revoke_races_command`、`E2.F14.revoke_propagation_lost` | 撤销送达与指令并发；撤销消息丢失 | 停止时刻不晚于撤销时刻 + `L` + 停止时间 | C-PERMIT-005、C-PERMIT-007 |
| `E2.F14.renewal_lost`、`E2.F14.delayed_renewal_replay` | 续租丢失；延迟的续租被重放 | 截止按 nonce 签发时刻计算；未知或淘汰的 nonce 被拒 | C-PERMIT-006、C-PERMIT-007 |
| `E2.F14.renewal_in_flight_timing` | 原截止 100 ms，91 ms 签出续租，92 ms 撤销，95 ms 续租到达 | 续租截止不晚于其 nonce 签发时刻 + `L`；停止不晚于撤销时刻 + `L` + 停止时间 | C-PERMIT-005、C-PERMIT-006 |
| `E2.F14.renewal_after_deadline` | 截止之后才到达的续租 | 被拒，会话不恢复，须以新 `permit_id` 重新准入 | C-PERMIT-008 |
| `E2.F14.kernel_restart`、`E2.F14.calibration_invalidated` | Kernel 重启后旧许可继续使用；执行端校准版本变化 | 代际或校准不一致即收敛停止 | C-PERMIT-004 |
| `E2.F14.estop_reset_replay` | 急停复位后重放旧许可 | 始终被拒，须所有者显式恢复并重新准入 | C-PERMIT-013 |
| `E2.F14.executor_restart_disabled` | 模拟执行端重启 | 启动默认失能，收到新签发的许可前不输出 | C-PERMIT-012 |
| `E2.F14.command_without_permit` | 绕过 Kernel 直接向执行端发送指令 | 被拒 | C-TRUST-004、C-PERMIT-007 |

## E3：重启后替换模型或策略

| 用例 ID | 场景与注入 | 主要断言 | 条款 |
|---|---|---|---|
| `E3.S1.baseline_checkpoint` | 完成一个可计数动作，保存共同检查点并保留暂停任务与未完成模型请求 | 存储确认后进入可重启点；各对象 ID、版本与消耗完整；同一快照下策略输出相同 | C-POLICY-006、C-PROV-012 |
| `E3.S2.swap_provider_late_old_result` | 重启配置从 Mock A 切到 Mock B，恢复后注入 Mock A 的晚到响应 | 旧代结果不改状态；旧调用按原绑定对账或保持未知 | C-CALL-011、C-ID-006、C-POLICY-010 |
| `E3.S2.unknown_call_not_rebound` | 切换时有一个结果未知的旧调用 | 不换绑定、不换 operation 重做 | C-CALL-008、C-ID-004 |
| `E3.S2.new_call_uses_new_binding` | 恢复后产生新的独立调用 | 新调用使用 Mock B；已有步骤的 `logical_call_id` 不变 | C-CALL-002、C-ID-003、C-POLICY-003 |
| `E3.S3.completed_operation_returns_old` | 恢复未完成工作，重查已完成 operation | 返回旧结果，计数不变，权限与私有记忆不扩大 | C-ID-003、C-POLICY-006 |
| `E3.S4.swap_strategy_at_checkpoint` | 在共同检查点重启，切换到另一兼容策略 | 目标、任务、授权、记忆与动作记录有效；新策略按 `step_key` 接续 | C-CALL-004、C-POLICY-008、C-POLICY-010 |
| `E3.S5.incompatible_private_checkpoint`、`E3.S5.unknown_strategy_version` | 注入不兼容的私有检查点；或未知策略版本 | 暂停并报告原因，保留原记录，不重置 Task | C-LIFE-024、C-POLICY-007、C-VER-030、C-CALL-004 |

E3 步骤 6（最小真实认证开发网关）不属于 M1 确定性夹具，M2 以脱敏证据单独记录（C-PROV-011）。

## E4：有界组合、订阅依赖与语义升级/回滚

| 用例 ID | 场景与注入 | 主要断言 | 条款 |
|---|---|---|---|
| `E4.S1.register_and_run` | 以公开接口注册 `composite_workflow` 与版本化步骤并执行 | Kernel 无业务分支改动；每步独立授权、调用身份与资源证据 | C-CALL-001、C-POLICY-003 |
| `E4.S1.mismatched_binding_rejected` | 提供错误模态、资源或外发目的地的 Binding | 在 `ayni_start` 或派发前拒绝 | C-BIND-001、C-PROV-009 |
| `E4.S1.schema_keyword_rejected` | 能力 Schema 使用子集之外的关键字 | 注册失败 | C-JSON-012 |
| `E4.S2.restart_at_each_boundary` | 第一步完成、后续抓拍与分析待决时，在每个提交边界重启并换兼容策略（每个边界一个用例） | 完成步骤跳过，未派发步骤重新准入；新尝试沿用 `logical_call_id`；计数不增加 | C-CALL-004、C-CALL-007、C-ID-003、C-POLICY-008 |
| `E4.S3.parent_cancel_propagates` | 父组合在子步骤在途时取消 | 取消向下传播；记录列出已完成、未执行与未知的步骤 | C-LIFE-011、C-LIFE-022、C-CALL-009 |
| `E4.S3.unknown_step_blocks_dependents` | 某步骤结果未知 | 依赖它的步骤不执行，不换 Provider 重做；无父子持锁死锁 | C-CALL-006、C-CALL-008 |
| `E4.S3.round_over_eight_rejected` | 一轮提交 9 个调用 | 整轮拒绝并报告原因，不执行前 8 个 | C-LIMIT-006 |
| `E4.S4.delete_during_analysis` | 分析在途时删除记忆或撤销共享 | 依赖复核拒绝旧摘要与动作建议 | C-PROD-030、C-COMMIT-012 |
| `E4.S4.subscription_closed_old_generation` | 分析在途时关闭订阅或修改观察版本 | 旧代事件失效，停止后无新观察触发 | C-SUB-002 |
| `E4.S5.v2_only_pauses_old_operation` | 存在旧操作时只注册不兼容的 v2 | 缺少旧语义时暂停，不用 v2 解释 v1 参数 | C-VER-011、C-VER-030 |
| `E4.S5.resolver_reconciles_v1` | 再提供旧版本查询 resolver | 补得原操作结果，不执行新动作 | C-VER-011 |
| `E4.S5.unknown_goal_type_rejected` | 提交未注册的 GoalType | 拒绝 | C-POLICY-009 |
| `E4.S6.migration_interrupted` | 存储迁移中断 | 从旧格式重新开始，校验通过后才切换 | C-VER-021、C-STORE-010 |
| `E4.S6.rollback_unreadable_state` | 新格式已写入新动作、结算与删除后回滚到模拟旧固件 | 旧固件进入安全诊断，不回到旧快照执行；动作不重复、消费不回退、删除不复活 | C-VER-020、C-VER-022、C-STORE-004 |
| `E4.S6.compat_matrix_matches` | 按升级清单的兼容矩阵逐项运行 | 实际行为与矩阵一致 | C-VER-031 |
| `E4.S7.queued_not_presented_restart` | 通知已入列、未呈现时重启 | 保持待呈现 | C-PROD-010、C-PROD-013 |
| `E4.S7.present_success_commits_with_task` | 呈现器返回成功 | `notify.present` 的成功结果与 Task 完成同批提交，无需用户 ACK | C-PROD-011、C-PROD-015、C-PROD-017 |
| `E4.S7.present_unknown_pending` | 呈现器返回未知，随后回执重复 | 按 `RESULT_UNKNOWN` 待处理；重复回执不重复终结 | C-PROD-013 |
