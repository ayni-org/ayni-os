# 调用、绑定与订阅

- **关联**：计划 §2.7、§3.2「异步」、§3.6、§4.3；ADR-0004、ADR-0007、ADR-0021、ADR-0026；`MA-008`、`MA-009`、`MA-033`；E2 全部注入点，E3 步骤 2–4，E4 步骤 1–5
- **属主**：`work/call`、`work/action`、`work/subscription`（调用实例）；`catalog/capability`、`catalog/binding`（描述与选择约束）

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
| C-CALL-012 | 重复的完成事件按 `(boot_epoch, request_id)` 只应用一次，不重复终结、释放或应用结果 | 重复回执使 Task 进度更新两次 | 内部交接部分已实现：`ProviderMailboxTest.reserves_complete_delivery_capacity_before_submit_and_reuses_only_after_ack`、`ProviderMailboxTest.progress_is_copied_and_old_ack_cannot_erase_newer_progress`、`ProviderMailboxTest.acknowledged_cancel_is_not_accepted_twice_and_withdraw_cannot_discard_results`；Task/ActionRecord 原子应用仍待 MA-008；E1 步骤 10；E2 F11 |
| C-BIND-001 | 注册时核对 Binding 与 Capability 的类型、Schema/语义版本、调用形态、模态、资源与取消/查询特性；不匹配的 Binding 在 `ayni_start` 时拒绝 | 视觉分析能力绑定到只支持文本的 Provider | E4 步骤 1 |
| C-BIND-002 | 同名能力可以有多个 Binding；应用提供选择约束，Kernel 只在满足权限、外发与运行限制约束的候选中准入 | Kernel 选了未获外发授权的云端 Provider | `MA-008` |
| C-BIND-003 | 准备记录保存具体 Binding 及版本；改变已批准服务或数据目的地的 fallback 必须重新授权 | 主 Provider 失败后自动改用更贵的服务 | `MA-008`；C-RUN-005 |
| C-SUB-001 | 订阅在 `ACTIVE`（Provider 确认开始）之前不接纳业务观察 | 订阅请求刚发出就把事件写入上下文 | E1 步骤 8 |
| C-SUB-002 | 订阅事件按 `(subscription_id, generation, sequence)` 校验；旧代际或旧 `boot_epoch` 的事件只可用于必要的脱敏对账，不重新启用观察，也不进入新上下文 | 关闭观察后迟到的旧帧触发了一次分析 | E1 步骤 8；E4 步骤 4 |
| C-SUB-003 | 订阅续租重新检查权限与运行限制；续租失败立即停止新采集与上传；Provider 未确认结束时资源按原规则释放或隔离 | 撤销授权后订阅凭旧租期继续上传 | E1 步骤 8 |
| C-SUB-004 | 每类事件声明是否可合并、可丢弃及 gap 行为；遥测不承诺无限历史，控制事件不使用遥测的丢弃策略 | 队列满时丢弃存储确认事件 | M1 已实现（本地输入不合并、不丢弃，队列满时返回 `BUSY`，ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`IngressQueue.full_queue_rejects_other_events_with_busy`；遥测的合并与丢弃见 C-LIMIT-008，订阅事件的 gap 随 `MA-018`；E1 步骤 10；C-HOST-006 |
