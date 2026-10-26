# 调用、绑定与订阅

- **关联**：计划 §2.7、§3.2「异步」、§3.6、§4.3；ADR-0004、ADR-0007、ADR-0021、ADR-0026；`MA-008`、`MA-009`、`MA-033`；E2 全部注入点，E3 步骤 2–4，E4 步骤 1–5
- **属主**：`work/call`、`work/action`、`work/subscription`（调用实例）；`catalog/capability`、`catalog/binding`（描述与选择约束）

当前实施边界（ABI 0.31 / ADR-0045）：公开 Provider/Binding 注册、有限 typed 参数与 UTF-8 正文、统一调用身份/资源/屏障、可靠完成、Task RUNNING/CANCELLING、所有者主动对账已接入 Kernel。journal/snapshot 共用完整调用记录，默认配置不启用调用容量。本轮验证限正常路径和公开边界；新故障/恢复专项、订阅、嵌套工具结果及长期记忆删除仍待后续包。ABI 0.35 的物理 Mock 许可及最小观察前提见 permits_and_stop.md；ProviderRequest 新增只在 submit 内借用的 execution_permit，非物理请求为 null，旧 0.x 消费者须重编译。

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
| C-CALL-001 | 每个 Capability 声明调用形态、输入/结果 Schema、副作用属性、所需资源、时限与恢复属性；统一入口不接受任意字符串代码或无 Schema 的字典 | 能力以 `{"cmd": "..."}` 的自由字符串执行 | E4 步骤 1；`MA-008` |
| C-CALL-002 | 逻辑调用独立于 I/O 尝试：一个 `logical_call_id` 可以有多个 `(boot_epoch, request_id)` 尝试，结果以尝试身份关联 | 每次重试分配新 `logical_call_id`，调用计数与账本各记一次 | E2 F7；E3 步骤 2 |
| C-CALL-003 | Host/Provider 的 `submit` 返回成功只表示 I/O 已受理，不表示业务成功；结果经事件回送 | 把 submit 返回 OK 当作提醒已呈现 | E1 步骤 11；`MA-008` |
| C-CALL-004 | 共同检查点保存步骤版本、绑定、输入摘要、结果引用、已提交进度与未知状态；策略私有检查点只保存算法状态；共同检查点不兼容时 Task 暂停 | 新策略改写共同检查点中的完成条件 | M1 已实现（已提交进度：步骤由 Kernel 保存，策略只能以 `REPORT_PROGRESS` 追加；私有检查点与步骤分开，只保存策略返回的状态，ADR-0024）：`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`、`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`、`TaskTable.decisions_commit_steps_checkpoints_and_progress`；步骤版本、绑定、输入摘要、结果引用与未知状态随 `MA-008`，共同检查点的版本不兼容时暂停也随之；重启后私有检查点不兼容的 Task 停下，步骤保留（ADR-0026 决策 7）：`WorkRecovery.task_its_strategy_cannot_serve_is_held`；E3 步骤 4–5；E4 步骤 2 |
| C-CALL-005 | 有副作用（任一非 `NONE` 类别达到门槛）的调用使用 ActionRecord、`operation_id` 与派发屏障；纯计算不需要动作账本，但计费模型请求与媒体上传仍持久绑定调用、Task 调用计数与外发证据 | 计费的模型请求没有持久记录，重启后重发导致双重计费 | E2；E1 步骤 9 |
| C-CALL-006 | 首版只支持有界顺序组合：单轮最多 8 个调用；组合不持有父级资源等待子调用；未知步骤阻止依赖它的后续步骤 | 转向结果未知时继续拍照并据此分析 | E4 步骤 3；C-LIMIT-006 |
| C-CALL-007 | 已完成步骤不重做，未派发步骤重新准入，补偿作为新调用单独授权 | 恢复后重新执行已完成的转向 | E4 步骤 2 |
| C-CALL-008 | 越过派发屏障后禁止静默切换实现；未知结果不能通过更换 Provider 或 `operation_id` 绕开；换绑定须先取得权威未执行结论并封禁旧 ID，再以新 `operation_id` 重新准备与授权 | 原 Provider 超时后改用备用 Provider 立即重发 | E3 步骤 2；E4 步骤 3 |
| C-CALL-009 | 取消请求携带目标操作及版本；Provider 报告“取消成功”“无法取消”或“结果未知”；资源在确认释放或隔离后才处理 | 取消请求发出即释放执行器资源 | E1 步骤 4；E4 步骤 3 |
| C-CALL-010 | 只有执行方声明 `AUTHORITATIVE` 查询、保证记录完整，且旧尝试已终止或被隔离时，“未执行”才是权威结论；`NOT_FOUND` 或无记录不能推断未执行 | 查询返回“无记录”后重新派发 | E2 查询夹具（两种响应） |
| C-CALL-011 | 同 boot 内迟到的结果可以用于对账、费用事实与停止确认，但不重新开放已取消的 Task；旧 boot 的结果须经权威查询入口对账，不直接修改新状态 | 重启前发出的模型请求结果复活了已取消的任务 | E2 F7；E3 步骤 2 |
| C-CALL-012 | 重复的完成事件按 `(boot_epoch, request_id)` 只应用一次，不重复终结、释放或应用结果 | 重复回执使 Task 进度更新两次 | 内部交接部分已实现：`ProviderMailboxTest.reserves_complete_delivery_capacity_before_submit_and_reuses_only_after_ack`、`ProviderMailboxTest.progress_is_copied_and_old_ack_cannot_erase_newer_progress`、`ProviderMailboxTest.acknowledged_cancel_is_not_accepted_twice_and_withdraw_cannot_discard_results`；公开异步与取消应用：`ManagedCall.async_worker_result_advances_the_same_task_and_preserves_copied_input`、`ManagedCall.cancellation_waits_for_terminal_and_late_success_does_not_reopen_task`、`ManagedCall.progress_is_monotonic_and_duplicate_terminal_is_rejected`；E1 步骤 10；E2 F11 |
| C-BIND-001 | 注册时核对 Binding 与 Capability 的类型、Schema/语义版本、调用形态、模态、资源与取消/查询特性；不匹配的 Binding 在 `ayni_start` 时拒绝 | 视觉分析能力绑定到只支持文本的 Provider | E4 步骤 1 |
| C-BIND-002 | 同名能力可以有多个 Binding；应用提供选择约束，Kernel 只在满足权限、外发与运行限制约束的候选中准入 | Kernel 选了未获外发授权的云端 Provider | `MA-008` |
| C-BIND-003 | 准备记录保存具体 Binding 及版本；改变已批准服务或数据目的地的 fallback 必须重新授权 | 主 Provider 失败后自动改用更贵的服务 | `MA-008`；C-RUN-005 |
| C-SUB-001 | 订阅在 `ACTIVE`（Provider 确认开始）之前不接纳业务观察 | 订阅请求刚发出就把事件写入上下文 | E1 步骤 8 |
| C-SUB-002 | 订阅事件按 `(subscription_id, generation, sequence)` 校验；旧代际或旧 `boot_epoch` 的事件只可用于必要的脱敏对账，不重新启用观察，也不进入新上下文 | 关闭观察后迟到的旧帧触发了一次分析 | E1 步骤 8；E4 步骤 4 |
| C-SUB-003 | 订阅续租重新检查权限与运行限制；续租失败立即停止新采集与上传；Provider 未确认结束时资源按原规则释放或隔离 | 撤销授权后订阅凭旧租期继续上传 | E1 步骤 8 |
| C-SUB-004 | 每类事件声明是否可合并、可丢弃及 gap 行为；遥测不承诺无限历史，控制事件不使用遥测的丢弃策略 | 队列满时丢弃存储确认事件 | M1 已实现（本地输入不合并、不丢弃，队列满时返回 `BUSY`，ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`IngressQueue.full_queue_rejects_other_events_with_busy`；遥测的合并与丢弃见 C-LIMIT-008，订阅事件的 gap 随 `MA-018`；E1 步骤 10；C-HOST-006 |

## ABI 0.31 的调用消费

策略在 `decide` 回调中使用 `output.request_call` 提交唯一 REQUEST_CALL/REQUEST_MODEL；sink 立即复制参数与文本，回调返回后失效。当前一个决定只接受一个调用，不能混入其他意图；Task 私有 checkpoint 与逻辑调用受理同批提交。`step_key` 在 Task 内唯一，失败/取消调用也占用有界步骤身份，不能借新失败步骤绕过 `task_step_capacity`。

CallRecord 保存原始有限参数域、规范输入、确切 Capability/Provider/Binding 版本和 operation。资源一次取得后先持久 PREPARED，再持久 DISPATCHING；最后复核 Task、授权、撤销 Gate、参数、资源健康及期限。Provider 回调只借用输入至返回，工作线程只保留自有副本和完成目标。模型次数在受理时增加一次；查询对账不增加执行次数。

终态结果、成功步骤及仍等待的 Task 进度在同一 B4 发布；正文须等结果批次持久后才可由 `ayni_copy_call_result` 对外复制。策略只能读取该 Task 的结果；管理查询为 owner 线程的本机受管 API。查询信息的 `requires_seq` 标出未持久事实，不是呈现许可。UNKNOWN 或未知影响会阻止 Task/Goal/调用记录回收。

`CALL_QUERY_RESULT` 可用于 UNKNOWN 结果及 FAILED/CANCELLED 的未知影响，使用可信所有者 MANAGEMENT 会话、非零 command_id 和独立 query_revision。用户确认即使执行 Grant 已撤销，所有者仍可查询原 Provider 的原 operation。重交相同命令返回原受理结果，不追加 I/O；新查询使用独立 attempt/代际和持久屏障。只有 AUTHORITATIVE 的 FOUND/NOT_ACCEPTED 可以解除 UNKNOWN；BEST_EFFORT 的 FOUND/无记录均保持未知。已取消 Task 不因对账结果恢复。重载后旧 boot 回执只能排空 transport，不能直接发布新 writer 的已知结果；该恢复路径尚未进行新专项验收。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-CALL-013 | 所有者管理查询原 operation 不依赖原执行 Grant；精确命令/CAS 去重，查询失败或弱证据不重试执行、不切换 Provider、不恢复取消 Task | 撤权后无法确认原动作；重复管理命令产生重复查询或查询成功复活取消任务 | 正常公开场景：`ManagedCall.owner_reconciles_after_grant_revocation_without_repeating_execution`、`ManagedCall.reconciled_result_resumes_only_the_still_waiting_task`、`ManagedCall.a_cancelled_task_stays_cancelled_when_reconciliation_finds_success`、`ManagedCall.query_requires_management_channel_exact_identity_and_current_query_revision`、`BestEffortCall.missing_record_never_proves_nonexecution`、`BestEffortCall.nonauthoritative_found_result_keeps_execution_unknown`；查询记录正常 codec：`CallTableTest.reconciliation_record_codec_preserves_query_generation_and_execution_identity`；重启/故障专项未验证 |

ABI 0.31 的普通资源等待超时只暂停原 Task，保留选定调用、输入和模型计数。所有者使用 `TASK_RESUME_WAIT`（非零 command_id、Task revision、MANAGEMENT 会话）显式重排队；普通 Goal/实例恢复不解除该暂停。解除等待不会解除用户暂停、授权或兼容限制；取消可以结束尚未派发的调用。正常验证：`ManagedCall.owner_resumes_the_same_resource_wait_without_another_model_charge`、`ManagedCall.wait_resume_preserves_user_pause_and_ordinary_resume_preserves_wait`、`ManagedCall.retained_resource_wait_can_be_cancelled_without_provider_dispatch`、`ManagedCall.wait_resume_never_bypasses_revoked_execution_authority`。新恢复/故障专项未执行。

## ABI 0.37 的类型化工具调用（ADR-0054）

GoalType 以 `tools` 声明最多 8 个可提议的工具 Binding（精确 ID 与版本，ID 不重复）。列出本身不授予任何权限；注册只检查引用的形状，`ayni_start` 再把每一项解析为策略可以请求的 TOOL 类 Binding（与提交时同一项派发检查），未知的返回 `AYNI_NOT_FOUND`，模型、订阅或无法派发的 Binding 返回 `AYNI_INVALID_ARGUMENT`，Kernel 留在 CONFIGURING。

结果格式为 `AYNI_CALL_RESULT_TOOL_CALLS` 的调用只能是非物理、非订阅的 MODEL 契约。派发时 Provider 得到这次调用可用的工具：允许列表中能对上 Task 所属 Goal 某一需求的 Binding（同 Capability 与版本、同来源域，且该需求的上限容许 Binding 自己的端点或目的地），即“允许列表 ∩ 已授权”。每个工具的视图给出字段的类型、单位和该上限内的取值；路由字段只列 Binding 自己的端点或目的地。视图回调只在本次 `submit` 内、在其线程上有效，任何 `submit` 之外返回 `AYNI_INVALID_STATE`；Provider 不保留它。

完成时 Provider 给出最终文本或 1～8 个类型化调用，二者择一；每个调用给出精确 Binding、每个字段恰好一个值和正文。形状错误或向文本格式调用发表列表在 `ayni_provider_post` 时返回 `AYNI_INVALID_ARGUMENT`，编码超出该调用的 `result_text_limit` 返回 `AYNI_CAPACITY_EXCEEDED`，借用保持。Kernel 在应用回执时按提交时的视图逐项复核；模型输出无法变成类型化结果（无法解码、未知工具、取值越界）时，Provider 应报告 `FAILED` 与 `AYNI_REASON_MODEL_OUTPUT`，由策略决定下一步。

调用列表随调用记录持久（type21/v4），策略经快照的 `call_tool` 读取本 Task 的提议，`ayni_call_info_v1.tool_call_count` 公开数量，`ayni_copy_call_result` 对列表复制 0 字节。Kernel 不执行提议：策略要执行某个提议，须在下一次决定中自己提交 REQUEST_CALL。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-CALL-014 | 模型调用只能提议本 Task 可用的工具：GoalType 允许列表中已被 Goal 授权需求覆盖的 Binding，取值在该需求的上限内；回执中视图之外的工具、取值、路由或超出工具输入上限的正文是无效证据，调用以 `ABANDONED`/`RESULT_UNKNOWN`/`PROVIDER_RESULT` 结束、Task 按未知调用暂停，不交给策略 | Provider 返回未提供的 `lamp`，或 45 分钟超出 Goal 的 30 分钟上限，策略据此执行 | M1 已实现：`ToolCalls.model_call_is_offered_only_listed_tools_within_the_goal_ceiling`、`ToolCalls.unoffered_tools_and_values_outside_the_offer_end_unknown`、`ToolCalls.malformed_lists_are_refused_on_post_and_keep_the_borrow`、`ToolAllowList.registration_checks_each_reference`、`ToolStart.start_needs_each_tool_to_be_an_ordinary_tool_binding`；Mock 模型输出失败：`MockTools.unoffered_tool_fails_as_model_output`、`MockTools.body_over_the_tool_limit_fails_as_model_output`、`MockTools.list_over_the_result_limit_fails_as_model_output` |
| C-CALL-015 | 提议的工具调用不自动执行：策略以新的 REQUEST_CALL 请求它，与任何请求一样经过 Binding、授权、资源和派发屏障；提议列表随调用记录持久，重启后仍可读取 | 收到模型提议后 Kernel 直接派发，或绕过 Grant 检查 | M1 已实现：`ToolCalls.strategy_requests_a_proposed_call_through_the_ordinary_checks`、`ToolCalls.proposed_calls_survive_restart_with_the_call_row`、`MockTools.proposes_the_offered_tool_at_its_first_values_and_the_proposal_runs`；记录与编码：`CallTableTest.tool_call_list_result_is_retained_and_round_trips_as_v4`、`ToolCallsTest.decoder_rejects_noncanonical_or_truncated_lists` |
