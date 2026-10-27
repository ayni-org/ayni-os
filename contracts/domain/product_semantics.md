# 五项产品语义

- **关联**：计划 §0、§2.4「操作传播」、§4.4「解绑」、§5.2、§5.4、§14.1；ADR-0007、ADR-0019、ADR-0020、ADR-0026；`MA-011`、`MA-019`、`MA-028`；E1 步骤 1、7–9、11，E4 步骤 4、7，第 10.7 节试用流程
- **属主**：应用层的助理协调器与 GoalType（`applications/`）负责路由、合并与呈现；Kernel 的 `work`（Goal/Task、调用）、`knowledge`（记忆、lineage、tombstone）与 `authority`（主体、Grant）负责其中的权威事实。协调器不直接修改 Kernel 对象（计划 §2.7）。

本文把计划 §14.1 已确认的五项产品语义写成可判定的条款。它们改变时必须先由用户确认（计划 §14.2 末段），不能在实现中悄悄调整。

| 编号段 | 产品语义 | 已确定行为（计划 §14.1） |
|---|---|---|
| 001–009 | 所有者与语音 | v0.1 单一所有者；按键语音沿用已有授权，新增权限等敏感操作经绑定管理端确认 |
| 010–019 | 提醒完成 | 按用户选定方式成功呈现才完成，无需用户确认；失败/未知待处理，仅入列不算完成；错过的提醒默认补发一次 |
| 020–029 | 暂停助理 | 保留到期提醒；暂停新自主观察、反思和运动并收敛在途，独立紧急停止另行恢复 |
| 030–039 | 忘记 | 源记录、可追溯派生摘要及在途依赖失效；已外发数据按保留说明处理 |
| 040–049 | 助理公共档案 | 所有者独立命名空间，按 Agent 授权访问；不改变各 Agent 默认私有 |
| 050–059 | 助理状态（支撑上述五项的呈现） | 并发事实的投影，不以一个枚举隐藏并行任务 |

## 通知的用户可见状态

用户可见状态是呈现调用事实的投影，不另存（C-PRIM-003）。Kernel 不设通知专用状态机。

| 用户可见状态 | 对应调用事实 |
|---|---|
| `AVAILABLE` | 通知内容与呈现步骤已持久接受，尚未派发；安静时段延后也停留于此 |
| `PRESENTING` | 呈现调用处于 `DISPATCHING` 或 `IN_PROGRESS` |
| `PRESENTED` | 呈现调用以成功证据 `COMPLETED` |
| `PRESENTATION_UNKNOWN` | 呈现调用处于 `RESULT_UNKNOWN` |
| `PRESENTATION_FAILED` | 呈现调用处于 `FAILED` |
| `ACKNOWLEDGED`（可选） | 用户确认事件；只作为确认型提醒 GoalType 的完成条件 |

## 暂停与停止的传播

| 操作 | 传播到 | 不影响 |
|---|---|---|
| 暂停助理 | 自主观察、反思与运动所属目标的新工作；取消相关订阅与可取消请求；收敛在途运动 | 已设提醒、用户明确创建的其他工作、紧急停止锁存 |
| 关闭观察 | 相应后台观察目标及订阅；停止采集、上传与新分析 | 用户日后独立发起的抓拍（新调用） |
| 紧急停止 | 运动准入与执行器输出（C-STOP-002） | 提醒、目标、记忆 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-PROD-001 | v0.1 每台设备只有一个所有者；所有者通过绑定管理端建立 UserPrincipal，是全部授权的根 | 第二个家庭成员在网页上注册后也能新增 Grant | `MA-028`；第 10.7 节（单所有者配网绑定） |
| C-PROD-002 | 语音请求需要尚未持有的权限时，助理说明原因并引导所有者在绑定管理端确认；语音请求本身不生成可由后续语音“确认”的待批授权 | 助理问“要允许上传吗？”，用户口头回答“好”，授权随即生效 | E1 步骤 7；C-AUTH-009 |
| C-PROD-003 | 解绑或所有权转移先停止敏感活动，撤销 Grant、管理会话与设备上的模型凭据，失效媒体引用，再按所有者确认的擦除或导出方案处理数据 | 解绑后旧所有者的模型凭据仍能调用模型 | `MA-028`；第 10.7 节（解绑） |
| C-PROD-004 | 新所有者不继承旧所有者的公共档案或任何私有记忆；结果未知的动作保留必要的脱敏账本，不因擦除而被当作从未执行 | 恢复出厂后新用户看到旧用户的偏好；未知动作的记录被一起清掉 | `MA-028` |
| C-PROD-010 | 普通提醒的一个 occurrence 只在其主呈现调用以成功证据 `COMPLETED` 时完成；不需要用户确认；入列、发送成功都不算完成 | 通知写入网页待展示队列后，提醒即标为完成 | M1 正常实现：`AssistantApplication.pausing_autonomy_preserves_reminders_and_new_goal_membership`、`UnavailableAssistantApplication.unsupported_channel_preserves_pending_reminder_and_projects_failure`；完整 E1/E4 未验收 |
| C-PROD-011 | 每个渠道的呈现是声明 `NOTIFICATION` 副作用类别的 `notify.present` 能力，经 ActionRecord、`operation_id`、派发屏障与该能力的去重/查询属性处理；呈现成功结果先持久保存，随后策略按原步骤完成 Task；两者之间可暂时显示已呈现/任务收尾中，不能重发原成功调用（用户 2026-10-27 选择，ADR-0066） | Kernel 为通知单独维护一个“已读/未读”状态机，与 ActionRecord 不一致 | M1 正常实现：`QuietAssistantApplication.blocked_writer_keeps_owner_responsive_and_queued_output_rechecks_time`；正常两步完成；ADR-0066；完整 E1/E4 未验收 |
| C-PROD-012 | 每个提醒明确一个主呈现方式与获准的备选方式，由 GoalType 的步骤序列表达；不暗含“所有渠道”，也不任意降级；获准列表之外的备选须重新授权 | 设备扬声器失败后，系统自动改用未获授权的外部推送服务 | `MA-019`；E1 步骤 11 |
| C-PROD-013 | 呈现失败或结果未知时，提醒保持待处理并显示 `PRESENTATION_FAILED` / `PRESENTATION_UNKNOWN`；已确认呈现的提醒重启后不再播报；未确认的按渠道能力的去重/查询属性对账或标为待处理，不自动重复不支持去重的播放 | 重启后不确定是否播过，于是把提醒再大声播一遍 | M1 正常实现：`AssistantApplication.presented_reminder_is_not_replayed_after_orderly_close_and_reopen`、`UnavailableAssistantApplication.unsupported_channel_preserves_pending_reminder_and_projects_failure`；新增故障/恢复专项未验证；完整 E1/E4 未验收 |
| C-PROD-014 | 每个 occurrence 的呈现调用使用稳定的 `operation_id`；重复的呈现回执不重复完成 | 网页刷新两次各发一次回执，周期提醒的两个 occurrence 都被标为完成 | E1 步骤 11（回执重复）；C-CALL-012 |
| C-PROD-015 | 设备声光与音频由输出完成证据确认；网页由已认证客户端的呈现回执确认；呈现能力声明独立的截止时间，不沿用通用请求超时 | 网页回执在用户 2 小时后打开页面时到达，被 30 秒请求超时判为失败 | `MA-019`；E4 步骤 7 |
| C-PROD-016 | 安静时段中只记录未呈现的提醒保持 `AVAILABLE`，按已选的错过/到期策略延后或明确过期，不标为完成；v0.1 默认不在安静时段大声播报普通语音，停止与故障指示照常生效 | 安静时段跳过播报后把提醒标为“已完成” | E1 步骤 11（安静时段） |
| C-PROD-017 | 用户确认收到不是普通提醒完成的前置条件；需要确认的提醒是另一种 GoalType，其完成条件是 `ACKNOWLEDGED` | 普通提醒一直等用户点击“知道了”才完成 | E4 步骤 7（无需用户 ACK） |
| C-PROD-018 | 普通提醒的默认错过策略是“补发一次”（计划 §14.1，v1.7 用户决定）：因断电、恢复或等待校时（C-TIME-004）而错过原定时间的 occurrence，在恢复并取得可信时间后呈现一次，并标明原定时间；周期提醒错过多个 occurrence 时只补发一次，不逐个补发（C-LIMIT-007）。“汇总”与“跳过”是 GoalType 可以另选的策略，不是默认值 | 断电一晚后重启，每小时一次的喝水提醒连续播报 8 遍；或错过的提醒被静默丢弃，用户不知道它没有呈现 | `MA-019`；`MA-011`；E1 步骤 11。Kernel 侧的合并：等待的 occurrence 合并为一个 Task，记下原定时间与代表的个数（ADR-0019）`WorkTrigger.occurrences_missed_between_steps_become_one_task`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`；呈现与可信时间随 `MA-019`、`MA-011`。单调时钟上的触发在重启后不补发，从恢复时刻按完整时长重新计时，断电期间到期的 occurrence 推迟而不丢失（ADR-0026 决策 6）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`；依据可信时间的“补发一次”随 `MA-019`。Task 槽位不足时 occurrence 的等待上限由 GoalType 声明（ADR-0020），`MA-019` 的普通提醒须声明无界或足够长的等待，补发一次才不受容量影响 |
| C-PROD-020 | “暂停助理”只映射到自主观察、反思与运动所属目标的新工作；已设提醒与用户明确创建的其他工作继续；界面明确显示“提醒继续”；不映射为暂停全部实例 | 暂停助理时把提醒所属的实例一并暂停，到期提醒没有呈现 | M1 正常实现：`AssistantApplication.pausing_autonomy_preserves_reminders_and_new_goal_membership`；完整 E1/E4 未验收 |
| C-PROD-021 | 暂停助理取消相关订阅与可取消请求，收敛在途运动；恢复助理时重新准入，不续跑暂停前的运动 | 恢复助理后底盘继续执行暂停前没转完的动作 | M1 正常实现：`AssistantApplication.paused_analysis_round_is_retained_and_new_generation_continues`；真实运动停止不在本次应用范围；完整 E1/E4 未验收 |
| C-PROD-022 | 助理状态单独显示紧急停止；“恢复助理”不清除停止锁存，紧急停止只能按 C-STOP-002 显式恢复 | 用户点“继续”后紧急停止的提示消失，底盘可以运动 | 第 10.7 节（独立停止/恢复）；C-STOP-002 |
| C-PROD-023 | 关闭观察终止相应后台观察目标与订阅，停止采集、上传与新分析；旧代际事件不进入上下文；之后用户独立发起的抓拍是新调用 | 关闭观察后，迟到的旧帧又触发了一次云端分析 | E1 步骤 8；C-SUB-002 |
| C-PROD-030 | 删除记忆时，源记录及其可追溯派生摘要在同一批次（B5）中变为不可检索、不可进入新上下文，依赖它们的在途整理与动作建议同时失效；正文由可恢复的有界清理任务随后清除 | 删除后正文已清，但由它生成的摘要仍出现在下一次对话上下文中 | M1 正常路径已实现，见 C-MEM-008/011；E1 步骤 9、E4 步骤 4 和新故障恢复专项未验收；C-COMMIT-005 |
| C-PROD-031 | 结果应用时检查 tombstone、`expected_revision` 与授权版本；旧模型结果不能复活已删除内容，也不能覆盖用户的新修改 | 删除前发出的整理请求返回后，把已删除的事实写回摘要 | M1 正常路径已实现，见 C-MEM-005/008；E1 步骤 9 完整竞态未验收；C-COMMIT-012 |
| C-PROD-032 | 派生摘要保存有界 lineage；依赖超过上限时缩小输入或拒绝生成，不得截断依赖后仍宣称支持完整删除 | 摘要依赖 500 条记录，只记录前 32 条，删除第 100 条后摘要仍保留其内容 | M1 已实现：`MemoryFacts.dependency_union_is_atomic_and_never_truncates`；C-MEM-005、C-LIMIT-012 |
| C-PROD-033 | 已外发给外部服务或模型的数据按保留说明处理；系统不承诺撤回模型已收到的内容；已发生的影响与可选 usage 诊断如实保留（ADR-0042，无费用结算表）；审计只留脱敏引用 | 删除确认页写着“已从所有地方彻底删除” | `MA-027`；第 10.7 节（删除记忆与派生摘要） |
| C-PROD-034 | 混合摘要失效后可以只用仍有效的来源重新生成；删除不禁止用户之后主动提供独立的新记录 | 删除一条偏好后，用户再次说出同样的偏好也被拒绝保存 | M1 已实现：`NativeAssistant.deleting_one_turn_preserves_other_users_and_rebuilds_summary_from_valid_inputs`；C-MEM-010 |
| C-PROD-035 | 敏感删除须有已绑定管理端的认证证据 | 语音说“删掉所有记忆”就清空了公共档案 | M1 已实现所有者会话边界：`KernelMemory.management_authentication_applies_to_queries_and_exact_retries`；远程管理端仍随 MA-027；E1 步骤 7、C-AUTH-009 |
| C-PROD-040 | 助理公共档案是归属所有者的独立命名空间，按 Agent 授予读、写、用途与期限；不是所有 Agent 自动可读；各 Agent 的记忆仍默认私有 | 新注册的 Agent 自动读取公共档案中的全部偏好 | M1 部分实现（管理保存、按 Agent/用途读，独立写档案仅所有者）：`NativeAssistant.profile_starts_empty_and_grant_to_a_does_not_authorize_b`；C-MEM-003/004；E1 步骤 7 完整场景未验收 |
| C-PROD-041 | 用户明确让统一助理保存的偏好进入公共档案；私有记忆的共享与外发仍分别授权，共享一条不意味着共享整个私有库 | 用户共享一条私有事实后，另一个 Agent 可以检索该 Agent 的全部私有记录 | M1 部分实现（CLI 明确保存基础偏好、逐记录共享与外发）：`NativeAssistant.sharing_private_memory_then_revoking_blocks_reads_and_old_context`、`NativeAssistant.remote_memory_requires_separate_destination_grant_before_http_submission`；统一助理随 P5；E1 步骤 1、7 |
| C-PROD-042 | 后台整理不能读取其他 Agent 的私有记录，也不能把未经确认的推断改为已确认事实 | 记忆整理把“可能喜欢咖啡”的推断写成已确认偏好 | M1 部分实现（组织用途拒绝跨 Agent 私有读、模型/派生确认保持来源）：`MemoryFacts.confirmation_preserves_model_origin_and_source_dependencies`；后台整理调度随 P5；C-MEM-003/004；E1 步骤 1 |
| C-PROD-050 | 助理状态是并发事实的投影：停止与故障警示优先，前台聆听/处理显示当前交互，等待授权与后台目标另列；不用单个枚举隐藏并行任务 | 后台观察进行中时用户按键提问，界面只显示“后台观察” | M1 正常实现：`AssistantPolicy.routing_encoding_quiet_time_and_conflict_projection_are_explicit`、`NativeAssistant.autonomous_goals_reopen_and_approve_through_the_public_application`；M1 文本状态；硬件停止指示未验收；完整 E1/E4 未验收 |
| C-PROD-051 | 普通界面不显示内部 task ID、Provider 与资源锁等开发术语；这些只出现在开发诊断页 | 试用界面显示“blocked_on: model_slot#0” | `MA-028`；第 10.7 节 |
