# 调用、绑定与订阅

- **关联**：计划 §2.7、§3.2「异步」、§3.6、§4.3；ADR-0004、ADR-0007、ADR-0021、ADR-0026、ADR-0096；`MA-008`、`MA-009`、`MA-033`；E2 全部注入点，E3 步骤 2–4，E4 步骤 1–5
- **属主**：`work/call`、`work/action`、`work/subscription`（调用实例）；`catalog/capability`、`catalog/binding`（描述与选择约束）

当前实施边界（ABI 0.31 / ADR-0045）：公开 Provider/Binding 注册、有限 typed 参数与 UTF-8 正文、统一调用身份/资源/屏障、可靠完成、Task RUNNING/CANCELLING、所有者主动对账已接入 Kernel。journal/snapshot 共用完整调用记录，默认配置不启用调用容量。本轮验证限正常路径和公开边界；后续已交付范围见各 ABI 段；新故障/恢复专项仍暂停。ABI 0.35 的物理 Mock 许可及最小观察前提见 permits_and_stop.md；ProviderRequest 新增只在 submit 内借用的 execution_permit，非物理请求为 null，旧 0.x 消费者须重编译。

## 调用形态

| 形态 | 最小生命周期与确认 | 取消与恢复边界 |
|---|---|---|
| `unary` | 接受、一个终态结果；查询也可异步 | 可取消不等于未计费或未外发；重试按能力属性判断 |
| `action` | 接受、可选有序进度、终态与副作用状态；物理停止另行确认 | 取消是请求；已部分执行和未知必须保留事实 |
| `subscription` | `STARTING → ACTIVE → STOPPING → STOPPED`，另有 `EXPIRED`、`FAILED`；带 `subscription_id`、`generation`、租期与事件 `sequence` | 续租重新检查权限与运行限制；停止后的尾部事件不进入新上下文；重启以新代际重新准入 |

## 能力的副作用属性

每个 Capability 分别声明以下属性；它们互相独立，不能从一项推出另一项。

| 属性 | 取值 | 用途 |
|---|---|---|
| 副作用类别（可多选） | `NONE`、`DATA_EGRESS`（数据外发）、`BILLING`（计费）、`NOTIFICATION`、`PHYSICAL` | 是否需要动作账本与派发屏障 |
| 语义幂等 | `IDEMPOTENT`、`NOT_IDEMPOTENT` | 同一参数重复执行是否等价 |
| 服务端去重窗口 | 无，或时长与键（`operation_id`） | 能否以同一 ID 安全重发 |
| 查询权威性 | `NONE`、`BEST_EFFORT`、`AUTHORITATIVE`（执行方保证记录完整） | 能否形成权威未执行结论 |
| 取消方式 | `NONE`、`REQUEST`、`CONFIRMED` | 取消回执的含义 |
| 补偿能力 | 无，或补偿能力的引用 | 补偿是另一次独立授权的调用 |
| 呈现/执行截止 | 时长 | 例如网页呈现回执允许较长的 `IN_PROGRESS` |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-CALL-001 | 每个 Capability 声明调用形态、输入/结果 Schema、副作用属性、所需资源、时限与恢复属性；统一入口按已注册 Schema 解释正文；Shell 仅由可信部署显式授权 | 未注册 Schema/权限的 `{"cmd": "..."}` 直接执行 | E4 步骤 1；`MA-008` |
| C-CALL-002 | 逻辑调用独立于 I/O 尝试：一个 `logical_call_id` 可以有多个 `(boot_epoch, request_id)` 尝试，结果以尝试身份关联 | 每次重试分配新 `logical_call_id`，调用计数与账本各记一次 | E2 F7；E3 步骤 2 |
| C-CALL-003 | Host/Provider 的 `submit` 返回成功只表示 I/O 已受理，不表示业务成功；结果经事件回送 | 把 submit 返回 OK 当作提醒已呈现 | E1 步骤 11；`MA-008` |
| C-CALL-004 | 共同检查点保存步骤版本、绑定、输入摘要、结果引用、已提交进度与未知状态；策略私有检查点只保存算法状态；共同检查点不兼容时 Task 暂停 | 新策略改写共同检查点中的完成条件 | M1 已实现（已提交进度：步骤由 Kernel 保存，策略只能以 `REPORT_PROGRESS` 追加；私有检查点与步骤分开，只保存策略返回的状态，ADR-0024）：`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`、`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`、`TaskTable.decisions_commit_steps_checkpoints_and_progress`；步骤版本、绑定、输入摘要、结果引用与未知状态随 `MA-008`，共同检查点的版本不兼容时暂停也随之；重启后私有检查点不兼容的 Task 停下，步骤保留（ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`；E3 步骤 4–5；E4 步骤 2 |
| C-CALL-005 | 有副作用（任一非 `NONE` 类别达到门槛）的调用使用 ActionRecord、`operation_id` 与派发屏障；纯计算不需要动作账本，但计费模型请求与媒体上传仍持久绑定调用、Task 调用计数与外发证据 | 计费的模型请求没有持久记录，重启后重发导致双重计费 | E2；E1 步骤 9 |
| C-CALL-006 | 首版工具提议按有界顺序处理：单轮最多 8 个调用；每 Task 同时至多一个 TOOL action（排队/取消/UNKNOWN/借用隔离均占用），可与模型及订阅并存；组合不持有父级资源等待子调用；未知步骤阻止依赖它的后续步骤 | 转向结果未知时继续拍照并据此分析 | E4 步骤 3；C-LIMIT-006 |
| C-CALL-007 | 已完成步骤不重做，未派发步骤重新准入，补偿作为新调用单独授权 | 恢复后重新执行已完成的转向 | E4 步骤 2 |
| C-CALL-008 | 越过派发屏障后禁止静默切换实现；未知结果不能通过更换 Provider 或 `operation_id` 绕开；换绑定须先取得权威未执行结论并封禁旧 ID，再以新 `operation_id` 重新准备与授权 | 原 Provider 超时后改用备用 Provider 立即重发 | E3 步骤 2；E4 步骤 3 |
| C-CALL-009 | 取消请求携带目标操作及版本；Provider 报告“取消成功”“无法取消”或“结果未知”；资源在确认释放或隔离后才处理 | 取消请求发出即释放执行器资源 | E1 步骤 4；E4 步骤 3 |
| C-CALL-010 | 只有执行方声明 `AUTHORITATIVE` 查询、保证记录完整，且旧尝试已终止或被隔离时，“未执行”才是权威结论；`NOT_FOUND` 或无记录不能推断未执行 | 查询返回“无记录”后重新派发 | E2 查询夹具（两种响应） |
| C-CALL-011 | 同 boot 内迟到的结果可以用于对账、费用事实与停止确认，但不重新开放已取消的 Task；旧 boot 的结果须经权威查询入口对账，不直接修改新状态 | 重启前发出的模型请求结果复活了已取消的任务 | E2 F7；E3 步骤 2 |
| C-CALL-012 | 重复的完成事件按 `(boot_epoch, request_id)` 只应用一次，不重复终结、释放或应用结果 | 重复回执使 Task 进度更新两次 | 内部交接部分已实现：`ProviderMailboxTest.reserves_complete_delivery_capacity_before_submit_and_reuses_only_after_ack`、`ProviderMailboxTest.progress_is_copied_and_old_ack_cannot_erase_newer_progress`、`ProviderMailboxTest.acknowledged_cancel_is_not_accepted_twice_and_withdraw_cannot_discard_results`；公开异步与取消应用：`ManagedCall.async_worker_result_advances_the_same_task_and_preserves_copied_input`、`ManagedCall.cancellation_waits_for_terminal_and_late_success_does_not_reopen_task`、`ManagedCall.progress_is_monotonic_and_duplicate_terminal_is_rejected`；E1 步骤 10；E2 F11 |
| C-BIND-001 | 注册时核对 Binding 与 Capability 的类型、Schema/语义版本、调用形态、模态、资源与取消/查询特性；不匹配的 Binding 在 `ayni_start` 时拒绝 | 视觉分析能力绑定到只支持文本的 Provider | E4 步骤 1 |
| C-BIND-002 | 同名能力可以有多个 Binding；应用提供选择约束，Kernel 只在满足权限、外发与运行限制约束的候选中准入 | Kernel 选了未获外发授权的云端 Provider | `MA-008` |
| C-BIND-003 | 准备记录保存具体 Binding 及版本；改变已批准服务或数据目的地的 fallback 必须重新授权 | 主 Provider 失败后自动改用更贵的服务 | `MA-008`；C-RUN-005 |
| C-BIND-004 | ABI 0.52 的可信 Binding 可把有界逻辑资源 ID 映射为实际 ID；未列项保持原 ID，不增删/合并需求或改变单位，重复键/实际目标拒绝。原 Call 保存有效实际组及原逻辑证明，派发核对完整原有效契约与精确端点；配置不因模型发现而改变 | 两实例被迫共用资源，或队列释放后换用另一端点/实际资源 | 正常公开验证：`BindingInstances.exact_routes_with_independent_resources_run_concurrently`、`BindingInstances.shared_resource_queues_second_instance_until_release`、`ProviderApi.binding_resource_map_rejects_bad_shapes_before_registration`、`ProviderApi.binding_map_requires_a_declared_logical_demand`、`ProviderApi.binding_map_cannot_merge_two_logical_demands`；本征边界：`CallContract.binding_resource_substitution_keeps_semantics_and_unit_multiplicity`；最小正常重开：`BindingInstances.orderly_reopen_retains_mapped_call_and_exact_result`、`MappedSubscription.orderly_reopen_keeps_original_actual_resource_group`、`MappedSubscription.changed_actual_group_holds_original_call_without_new_opening`；只读原操作查询按原逻辑证明及原路由匹配，不将实际资源 ID 与逻辑 offer 直接比较：`CallContract.saved_query_matches_logical_proof_and_route_with_actual_resources`；其他新故障/恢复专项未验证 |
| C-SUB-001 | 订阅归一个 Task；Provider 的可靠 STARTED 确认前 gate 关闭，普通前台确认与释放同批，受管订阅受理即可解除前台、但 gate 仍等可靠确认，ACTIVE 后可继续模型分析 | 请求刚发出就写上下文，或连续观察阻止 Task 分析 | 正常公开验证：`SubscriptionFixture.start_ack_precedes_samples_and_active_collection_allows_model_call`；完整 E1 未验证 |
| C-SUB-002 | 样本/媒体按 subscription、boot、全局不复用 generation、递增 sequence 校验；不同 Task 同源隔离；关闭立即使缓存与 Scene 旧引用不可用，正常重开原意图以新代际准入 | 旧帧触发分析，或关闭 A 的媒体清理 B | 正常公开验证：`SubscriptionFixture.task_pause_invalidates_cache_and_resume_opens_a_new_generation`、`SubscriptionFixture.two_tasks_share_capacity_but_keep_samples_media_and_close_isolated`、`SubscriptionFixture.orderly_restart_preserves_intent_and_rechecks_a_fresh_opening`；完整竞态/恢复专项未验证 |
| C-SUB-003 | 默认30秒、半租期由 Kernel 重查完整授权/运行限制/资源后续租；ACK 只确认请求的绝对截止。拒绝/暂停/结束立即关闭采样 gate、停止续租并请求关闭；UNKNOWN 归还 transport 后资源仍保留至最长已请求租期 | 延迟 ACK 从到达时重新计租；UNKNOWN 提前释放仍在采集的资源 | 正常公开验证：`SubscriptionFixture.half_lease_renewal_confirms_the_requested_absolute_deadline`、`SubscriptionFixture.explicit_strategy_close_releases_its_own_subscription_after_terminal`、`SubscriptionQueryFixture.unknown_retains_units_and_query_waits_until_the_longest_requested_lease`、`SubscriptionBackpressureFixture.one_completion_item_reports_the_stop_still_ready_after_cancel_was_sent`；完整 E1 未验证 |
| C-SUB-004 | 遥测传输 FIFO 满拒绝、不合并/淘汰；每 Host 源和订阅有可靠 gap 前沿。STARTED/RENEWED 与取消/终态独立可靠，不使用遥测策略 | 满队列丢弃租期确认，或 gap 丢失后继续动作 | 正常验证：`ObservationIngress.full_fifo_refuses_samples_but_preserves_gap_and_local_input`、`ProviderMailboxTest.exact_lease_ack_is_retained_independently_until_owner_commit`、`ProviderMailboxTest.renew_controls_require_the_next_explicit_request_and_keep_cancel_receipt`；完整并发专项未验证 |
| C-SUB-005 | 策略用 WAIT_SUBSCRIPTION 等待 Task 所有订阅的新样本或状态变化；WAITING 保存最近代际/序号/状态前沿，不轮询、不逐帧积压；续租 ACK 本身不唤醒。暂停保留前沿，终态清除；预留唤醒与最终取消的版本号 | 无输入仍反复调用模型，或管理操作消耗最终唤醒版本 | 正常验证：`SubscriptionFixture.waiting_task_wakes_once_for_the_latest_sample_and_subscription_state`、`SubscriptionFixture.model_busy_consumer_resumes_with_the_latest_sample_then_waits`、`SubscriptionFixture.orderly_restart_of_waiting_task_retains_frontier_and_follows_new_generation`、`TaskTable.subscription_wait_reserves_a_wakeup_and_the_final_cancel_at_revision_limit`；不宣称逐帧历史 |

## ABI 0.31 的调用消费

策略在 `decide` 回调中使用 `output.request_call` 提交唯一 REQUEST_CALL/REQUEST_MODEL；sink 立即复制完整规范业务值，回调返回后失效。当前一个决定只接受一个调用，不能混入其他意图；Task 私有 checkpoint 与逻辑调用受理同批提交。`step_key` 在 Task 内唯一，失败/取消调用也占用有界步骤身份，不能借新失败步骤绕过 `task_step_capacity`。

CallRecord 保存原始授权域、完整业务/执行契约证明、规范输入、确切 Capability/Provider/Binding 版本和 operation。资源一次取得后先持久 PREPARED，再持久 DISPATCHING；最后复核 Task、授权、撤销 Gate、参数、资源健康及期限。Provider 回调只借用输入至返回，工作线程只保留自有副本和完成目标。模型次数在受理时增加一次；查询对账不增加执行次数。

终态结果、成功步骤及仍等待的 Task 进度在同一 B4 发布；正文须等结果批次持久后才可由 `ayni_copy_call_result` 对外复制。策略只能读取该 Task 的结果；管理查询为 owner 线程的本机受管 API。查询信息的 `requires_seq` 标出未持久事实，不是呈现许可。UNKNOWN 或未知影响会阻止 Task/Goal/调用记录回收。

`CALL_QUERY_RESULT` 可用于 UNKNOWN 结果及 FAILED/CANCELLED 的未知影响，使用可信所有者 MANAGEMENT 会话、非零 command_id 和独立 query_revision。用户确认即使执行 Grant 已撤销，所有者仍可查询原 Provider 的原 operation。重交相同命令返回原受理结果，不追加 I/O；新查询使用独立 attempt/代际和持久屏障。只有 AUTHORITATIVE 的 FOUND/NOT_ACCEPTED 可以解除 UNKNOWN；BEST_EFFORT 的 FOUND/无记录均保持未知。已取消 Task 不因对账结果恢复。重载后旧 boot 回执只能排空 transport，不能直接发布新 writer 的已知结果；该恢复路径尚未进行新专项验收。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-CALL-013 | 所有者管理查询原 operation 不依赖原执行 Grant；精确命令/CAS 去重，查询失败或弱证据不重试执行、不切换 Provider、不恢复取消 Task | 撤权后无法确认原动作；重复管理命令产生重复查询或查询成功复活取消任务 | 正常公开场景：`ManagedCall.owner_reconciles_after_grant_revocation_without_repeating_execution`、`ManagedCall.reconciled_result_resumes_only_the_still_waiting_task`、`ManagedCall.a_cancelled_task_stays_cancelled_when_reconciliation_finds_success`、`ManagedCall.query_requires_management_channel_exact_identity_and_current_query_revision`、`BestEffortCall.missing_record_never_proves_nonexecution`、`BestEffortCall.nonauthoritative_found_result_keeps_execution_unknown`；查询记录正常 codec：`CallTableTest.reconciliation_record_codec_preserves_query_generation_and_execution_identity`；重启/故障专项未验证 |

ABI 0.31 的普通资源等待超时只暂停原 Task，保留选定调用、输入和模型计数。所有者使用 `TASK_RESUME_WAIT`（非零 command_id、Task revision、MANAGEMENT 会话）显式重排队；普通 Goal/实例恢复不解除该暂停。解除等待不会解除用户暂停、授权或兼容限制；取消可以结束尚未派发的调用。正常验证：`ManagedCall.owner_resumes_the_same_resource_wait_without_another_model_charge`、`ManagedCall.wait_resume_preserves_user_pause_and_ordinary_resume_preserves_wait`、`ManagedCall.retained_resource_wait_can_be_cancelled_without_provider_dispatch`、`ManagedCall.wait_resume_never_bypasses_revoked_execution_authority`。新恢复/故障专项未执行。

## ABI 0.37 的类型化工具调用（ADR-0054）

GoalType 以 `tools` 声明最多 8 个可提议的工具 Binding（精确 ID 与版本，ID 不重复）。列出本身不授予任何权限；注册只检查引用的形状，`ayni_start` 再把每一项解析为策略可以请求的 TOOL 类 Binding（与提交时同一项派发检查），未知的返回 `AYNI_NOT_FOUND`，模型、订阅或无法派发的 Binding 返回 `AYNI_INVALID_ARGUMENT`，Kernel 留在 CONFIGURING。

结果格式为 `AYNI_CALL_RESULT_TOOL_CALLS` 的调用只能是非物理、非订阅的 MODEL 契约。派发时 Provider 得到这次调用可用的工具：允许列表中能对上 Task 所属 Goal 某一需求的 Binding（同 Capability 与版本、同来源域，且该需求的上限容许 Binding 自己的端点或目的地），即“允许列表 ∩ 已授权”。每个工具的视图给出字段的类型、单位和该上限内的取值；路由字段只列 Binding 自己的端点或目的地。视图回调只在本次 `submit` 内、在其线程上有效，任何 `submit` 之外返回 `AYNI_INVALID_STATE`；Provider 不保留它。

完成时 Provider 给出最终文本或 1～8 个原生 function 提议，提议可以附同一 assistant 文本。每项保留 function_name、协议 ID 与无损参数树或非法参数原文；这里只校验提议形状和容量，不验证业务 Binding/Schema/Grant。post 拒绝非法外层形状或超限时保持借用。提议以 ATC3 随 CALL_STATE type21/v1 持久；自 ABI 0.54，结果 `text` 即该列表编码，或经正文引用存放在 Host 正文存储中，策略用纯函数 `ayni_tool_calls_read`/`ayni_tool_call_read` 读取（ADR-0096）；Kernel 不自动执行。协议 ID 配对消息，source Model Call+提议序号保证 Ayni 步骤身份。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-CALL-014 | 目录只暴露 Task 策略与当前授权的交集；模型提议的未知 function/Binding、非法参数和容量错误返回匹配 tool 错误，无业务 Call 被接受时不得伪造 UNKNOWN。实际 Call 再做原 Schema、完整 Grant、精确路由和来源复核；模型上下文来源失效时正文不可消费 | 读目录即扩权；无业务执行却记作未知副作用 | `ToolCalls.task_directory_exposes_currently_authorized_adopted_metadata`、`ToolCalls.unknown_functions_and_invalid_business_inputs_do_not_forge_unknown_actions`、`ToolCalls.malformed_lists_are_refused_on_post_and_keep_the_borrow`、`MockTools.unknown_binding_is_a_normal_function_refusal_without_business_execution`、`MockTools.body_over_the_tool_limit_is_refused_after_known_model_completion` |
| C-CALL-015 | 提议的工具调用不自动执行：策略以新的 REQUEST_CALL 请求它，与任何请求一样经过 Binding、授权、资源和派发屏障；提议列表随调用记录持久，重启后仍可读取 | 收到模型提议后 Kernel 直接派发，或绕过 Grant 检查 | M1 已实现：`ToolCalls.strategy_requests_a_proposed_call_through_the_ordinary_checks`、`ToolCalls.proposed_calls_survive_restart_with_the_call_row`、`MockTools.generic_function_arguments_run_through_original_schema_and_authority`；记录与编码：`CallTableTest.tool_call_list_result_is_retained_and_round_trips_as_v4`、`ToolCallsTest.persisted_lists_require_exact_framing_and_supported_source_trees` |

## ABI 0.43 的 Task 订阅（ADR-0063）

SUBSCRIPTION 仅用于 TOOL、非 PHYSICAL、CONFIRMED 取消契约；静态绑定 source/version 和有限 lease（0选择默认30秒）。注册时 Provider 的续租端口和每个 offer 必须匹配。策略仍以 REQUEST_CALL 打开。STARTED 使开启动作成为已提交步骤，同时释放 Task.current_call；后台订阅继续占原资源，每代各有一次派发 claim 和持久屏障。订阅 ID 是原逻辑 Call，当前 generation 同时是该代 operation；查询只指向当前代，无自动重试 UNKNOWN。source 的 trusted Host 槽与各 Task scoped 槽分开，Scene 在该 Task 范围选取最新代际，不能从另一 Task 借观察权限。

策略通过 Task snapshot 的 subscription/subscription_observation 读取脱敏状态和最新样本。CLOSE_SUBSCRIPTION、WAIT_SUBSCRIPTION 都通过 request_call 暂存，只填 kind/step_key，其余业务字段清零；它们各自是唯一意图，不接受直接伪造的 policy intent。关闭不产生新调用；等待只写 Task 状态及检查点。WAITING 的新样本在 owner 消费后唤醒一次；忙时样本覆盖知识表最新值，FIFO 本身仍不覆盖。缓存过期本身和同状态续租 ACK 不触发决定；策略必须检查 usable。管理暂停或撤权先拒绝新样本，随后有界推进每条订阅停止；重新授权独立于用户恢复。正常停机/暂停后，已知 CANCELLED 的 preserved intent 复核权限和资源，以新代际重新开；FAILED、EXPIRED 和 UNKNOWN 不自动重开。

Provider 的 submit/renew/cancel 在 owner 线程同步调用且必须有界、不阻塞；只能 post 完成或样本，不能重入 owner API。工作线程持有自有参数副本及原 opening/attempt，任意非 ISR 线程 post 时复制所有短期视图；全部生产者须在 Kernel 销毁前 join。STARTED/RENEWED 独立可靠，必须回显预先请求的 kind/sequence；迟到或关闭后的 ACK 只排空，不复活 gate。Provider 自身必须在有限 valid_until 停止采集，即使续租/关闭丢失。Core 立即拒绝消费不能撤回已外发数据；媒体读、外发、保留仍分别授权。真实采集/硬件在 M3，新的故障/恢复专项仍未执行。

ABI 0.45：Task-scoped 调用结果视图携带原账本 effect（ADR-0066），与所有者查询共用取值属主。CANCELLED 本身不证明无影响；应用仅在影响已知时中止本 round/occurrence，UNKNOWN 保留待处理。通知完成另要求 COMPLETE 呈现证据。正常取消/新代际继续见 `AssistantApplication.paused_analysis_round_is_retained_and_new_generation_continues`；新增故障专项未验证。

## ABI 0.48 业务契约与 Shell

通用值与数值规则见 [业务值契约](business_values.md)，本包不提供旧 fields+text 业务入口。普通 TEXT 也是显式 Schema；请求的旧 arguments/text 布局槽保留为必须零，Provider arguments 仅为 Core 派生的固定控制投影，执行正文唯一来自 value。协议 ID 独立保存，ToolLoop 用原 Call 证明中的字段名打印嵌套请求/结构结果，不从当前目录推断旧结果。

Native `system.shell` 为显式宽权限 ACTION/TOOL，使用正常 Core 授权、资源、持久屏障和异步完成链。固定 cwd 不构成沙箱；完整脚本/管道继承宿主 OS 权限，声明 EGRESS/BILLING、NOT_IDEMPOTENT、QUERY_NONE、CANCEL_REQUEST，不自动重试。配置约束并发、有限运行时间和 stdout/stderr 合计上限；结果以 base64 无损保存字节、独立 exit/signal/status。超时/输出限额不能虚报无效果，进程组取消不等同任意原生代码隔离。正常链证据：`KernelShell.explicit_authority_controls_real_pipeline_and_structured_result_delivery`、`ShellProcess.executes_pipeline_in_configured_directory_and_preserves_exit_and_streams`、`ShellProcess.binary_output_has_explicit_lossless_encoding`。固定模型入口及 Native 部署开关属 AP-03，不据本 Provider 声称已完成。

## ABI 0.49 按需工具绑定

AP-03 第一部分新增 [Task 能力目录与按需授权](capability_directory.md)：可选列表不阻塞缺权 Task，实际请求用一个完整 Grant，与 Call 接受同批绑定；已用 Grant 失效只暂停拥有引用的 Task，显式重授权保留原整份 Scope/精确 Binding。REQUIRED 需求与既有派发屏障/UNKNOWN/隐私保证保持，固定模型工具和 Shell 装配属下一部分。


## Task 所有的受管控制（ABI 0.51 / ADR-0086）

公开 `operations.h` 在策略回调内只读原 Call，控制通过唯一 policy intent 暂存；不建立另一操作表。模型保留四个函数，`ayni_call_capability` 的 start/read/wait/cancel/close 与普通 invoke 分支严格检查各自封套，引用是精确十进制字符串，不能当权限。见[接口与消费者设计](../../docs/architecture/ap05-managed-operations-design.md)。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-OP-001 | managed 只接受 TOOL action/subscription；受理步骤、原 Call/Grant/输入与检查点同批，受理不是成功。共同 attachment 归 Call，后台完成不触动其他前台；所有 action 入口共用占用准入 | start 偷换成另一次调用；普通 invoke 绕过已有 action；后台完成结束模型 | 正常软件公开路径：`ManagedOperations.action_progress_wait_and_original_typed_result`、`ManagedOperations.action_and_subscription_progress_do_not_end_the_foreground_model`、`ManagedOperations.queued_action_preserves_agent_fifo_and_owner_can_cancel_the_task`；新故障/恢复专项未验证 |
| C-OP-002 | 引用只对原 Task 生效；read 可刷新旧 opening，控制只认确切 opening。read 不执行远程查询；cancel/close 不证明终态，close 即刻禁用样本并永久收紧 desired；UNKNOWN 不重试 | 下一 Task 继续关闭上一 Task；CANNOT_CANCEL 被写成已停止 | `ManagedOperations.final_text_closes_original_task_operations_and_reference_is_scoped`、`ManagedOperations.finite_wait_times_out_without_cancelling_then_explicit_cancel`、`OperationTokens.exact_round_trip_rejects_overflow_leading_zeros_and_malformed_null_view` |
| C-OP-003 | wait 原子比较显式 after，过去变化立即返回，未来 boot/sequence/gap 或非法形状拒绝。未变才进 WAITING；同状态 renew 不唤醒；正数时长最多30000ms，到期只结束本次wait。有限deadline不饱和；恢复后旧deadline不复用 | read与wait之间的进度丢失；超时偷偷取消；续租驱动模型忙转 | `ManagedOperations.progress_before_wait_returns_immediately_and_future_frontier_is_refused`、`ManagedOperations.finite_wait_times_out_without_cancelling_then_explicit_cancel`、`BackgroundAction.resource_resume_readmits_the_same_call_without_a_foreground_link`、`BackgroundAction.pause_cancels_the_queued_call_and_clears_only_its_resource_overlay`、既有 `SubscriptionFixture.waiting_task_wakes_once_for_the_latest_sample_and_subscription_state`；新恢复专项未验证 |
| C-OP-004 | 样本正文限有效 BOOL/I64、完整来源/时间/质量/单位；无媒体。进入工具历史/模型/摘要时保存确切来源，接受、派发和消费复核；失效重建完整协议组及原操作事实，不重做已受理控制。最终文字完成 Task 并停止名下操作，借用仍待真实终态 | 把历史样本或派生答案交给下一 Task；最终答复后继续采集；来源重建丢失原操作引用 | `ManagedOperations.sample_wait_rebuild_keeps_current_reference_and_next_task_discards_sample_history`、`ManagedOperations.software_subscription_sample_is_readable_then_close_invalidates_context`、`TaskCallTest.background_unknown_release_preserves_an_existing_policy_hold`、`TaskCallTest.foreground_completion_preserves_another_calls_unknown_overlay`、`ManagedResultHarness.managed_read_preserves_reference_and_original_large_result_through_summary` |

Task.call_unknown 独立于策略、资源等待、兼容及模型上限；Call 协调层维护其同批关系，诊断优先策略/资源、兼容、UNKNOWN、模型上限。revision 准入从 Call 属主查询额外的 UNKNOWN/解除/结束预留，不复制后台操作列表。资源等待覆盖 Task，恢复时须有原 Task 的真实未派发 Call，另一前台模型不能作为替代证明。普通 invoke 的业务结果处理保持，所有启用 Call 的 Task 决策先等其前一提交持久，避免策略回调副作用后再因同一持久屏障拒收。
