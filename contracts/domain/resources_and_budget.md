# 资源与运行限制

- **关联**：计划 §2.2、§2.3、§5.1、§5.3；ADR-0001、ADR-0008、ADR-0020；`MA-004`、`MA-018`；E1 步骤 2–5、8、10，E2 F8、F11
- **当前范围**：ADR-0042（用户 2026-10-25）取消累计费用预算；本文件名保留以保持链接。旧 `C-BUDGET-*` 条款废止，Task 运行限制见 `runtime_limits.md`。
- **属主**：`resources/admission`、`resources/lease`（资源准入、排队、租期、隔离与运行时状态）；`work/task`（任务运行限制）。资源与运行限制的联合准入由 `kernel` 在同一 transition 中先判定、后应用。

## 资源的状态

| 运行时状态 | 含义 | 可分配 |
|---|---|---|
| `FREE` | 在线、健康、无占用 | 是 |
| `HELD` | 被一个或多个调用占用（共享资源在容量内可多个） | 共享资源在剩余容量内是 |
| `QUARANTINED` | 占用已结束但未确认安全释放，例如执行器尚未确认停止 | 否 |
| `OFFLINE` / `FAULTED` | Host 报告离线或健康检查失败 | 否 |

这些状态都是运行时状态，重启后由 Host 重新探测，不从持久记录恢复（C-PRIM-001）。

## 工作的优先级

| 级别 | 工作 | 等待超限时 |
|---|---|---|
| 0 | 本地停止路径 | 不排队（C-STOP-001） |
| 1 | 前台交互、已到期提醒 | 延后并向用户说明原因 |
| 2 | 普通工作 | 按工作声明延后、过期或暂停，并记录原因 |
| 3 | 后台反思 | 可以不运行；不得进入无原因的永久等待 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-RES-001 | 资源由 Kernel 按 Capability 声明的所需资源自动申请、协调和释放；Agent、策略与 Provider 不直接持有驱动锁 | 策略在回调里直接锁住电机驱动，另一个实例的停止确认无法取得资源 | E1 步骤 3；`MA-018` |
| C-RES-002 | 一次调用所需的资源组全有或全无地准入；不允许部分占用后等待其余资源 | A 占住麦克风等扬声器，B 占住扬声器等麦克风，二者互相等待 | M1 部分实现（资源仲裁模块；调用消费未接入）：`ResourceAdmissionTest.group_admission_is_atomic_and_held_units_do_not_overflow`；E1 多资源集成待 MA-008 |
| C-RES-003 | 资源与 Task 运行限制的联合准入在同一 transition 中先判定、后应用；任一项不满足时两者都不改变（ADR-0042） | 资源已标为占用，Task 调用次数检查失败后资源没有归还 | `MA-018` 单元测试；E1 步骤 5 |
| C-RES-004 | 准入按上表的优先级排序；同级按 Agent 实例轮转、实例内 FIFO；跳过不可准入的实例队首但不允许其后继越过；低优先级可使用没有高优先级可准入请求的资源，并记录等待时间（ADR-0042） | 后台反思持续占用模型槽，到期提醒的语音合成一直排不上 | M1 部分实现（资源模块）：`ResourceAdmissionTest.agents_rotate_and_each_agents_requests_remain_fifo`、`ResourceAdmissionTest.blocked_head_preserves_fifo_and_other_agents_can_use_free_resources`、`ResourceAdmissionTest.foreground_goes_first_without_preempting_existing_background_work`；Task/调用集成待 MA-008 |
| C-RES-005 | 每类工作声明等待上限与超限后的处理（延后、过期或暂停）及原因码；等待超限不得静默保留在队列中 | 资源被长期占用时，排队请求无期限等待且没有任何原因可查 | M1 已实现（occurrence 等待 Task 槽位或 ID：GoalType 声明等待上限，超限跳过并计数、记原因码，可声明无界等待；Goal 信息给出当前的等待原因，ADR-0020；有停下的 Task 的 Goal 同样等待并跳过，原因 `TASK_HELD`，ADR-0022；跳过最后一个 occurrence 且没有未结束的 Task 时 Goal 进入 `FAILED`，原因保留在 `skip_reason`，ADR-0023）：`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.used_up_ids_skip_with_their_reason`、`WorkWait.skipping_the_last_occurrence_fails_the_goal`、`WorkWait.skip_leaves_a_goal_with_a_live_task_active`、`WorkWait.zero_wait_skips_at_once`、`WorkWait.unbounded_wait_never_skips`、`WorkWait.wait_reason_reports_what_would_hold_the_next_occurrence`、`WorkDecision.held_task_keeps_its_goal_from_deriving_until_it_ends`、`WorkDecision.held_task_keeps_its_goal_waiting_with_room_in_the_table`；Task 执行中的等待（资源、运行限制）与延后、暂停两种处理随 `MA-018`（ADR-0022 决策 1）；E1 步骤 10 |
| C-RES-006 | 首版不强制抢占不可中断的物理动作；优先级不意味着可以中断已开始的物理副作用，只能排队、超时或经可取消操作释放 | 前台请求到达时直接夺走正在转向的底盘资源 | M1 部分实现（不抢占已有资源 claim）：`ResourceAdmissionTest.foreground_goes_first_without_preempting_existing_background_work`；真实物理动作未实现/验证 |
| C-RES-007 | 调用完成、失败、取消、超时或所属实例停止时释放其资源；释放与调用终态在同一 transition 中应用 | 调用超时后资源仍标为占用，后续请求永远无法准入 | E1 步骤 4 |
| C-RES-008 | 未确认已停止的执行器资源进入 `QUARANTINED`，不分配给下一请求；许可结束或租期结束不解除隔离，Provider 也不能无限续租；只有停止确认回到 ActionRecord 后才释放 | 许可到期后资源立即可分配，下一个动作在电机仍在转动时被准入 | M1 部分实现（占用结束后隔离，精确 call_id + 占用代际确认）：`ResourceAdmissionTest.actuator_quarantine_requires_the_exact_released_call_confirmation`、`ResourceAdmissionTest.old_claim_cannot_release_or_unquarantine_a_reacquired_call`；许可/ActionRecord 消费及物理停止尚未验证 |
| C-RES-009 | 共享资源在声明容量内允许并行；独占资源任一时刻至多一个占用者；轨迹记录足以解释准入顺序与最长等待 | 容量为 1 的模型槽同时有两个活跃请求 | M1 部分实现（有界容量与诊断）：`ResourceAdmissionTest.group_admission_is_atomic_and_held_units_do_not_overflow`、`ResourceAdmissionTest.health_does_not_clear_holders_and_shared_spare_units_remain_usable`；E1 集成轨迹待实现 |
| C-RES-010 | 媒体采集以有期限的系统订阅占用资源，不能永久持锁；视觉分析占模型槽，不必长期占用摄像头；播放与录音是否互斥由音频资源组声明 | 一次视觉分析请求在等待模型结果期间一直独占摄像头 | E1 步骤 3、8 |
| C-RES-011 | 模型槽被占用期间，不依赖模型的本地工作照常推进；Kernel 不因等待外部 I/O 阻塞 `step` | B 的模型请求延迟时，A 的本地按键事件也被卡住 | E1 步骤 2（本地事件在 10 次 `step` 内处理） |
| C-RES-012 | 每个控制域任一时刻只有一个 authority-owner Kernel；控制域的 `authority_epoch` 由 owner 与执行端分别持久化 | 两台设备各自认为自己是底盘的 owner，交替签发许可 | `MA-018`；`MA-038`；ADR-0001 |
| C-RES-013 | 资源描述只来自可信 Host 注册；应用不能伪造或注册系统资源，发现到的描述只是候选（C-TRUST-002） | 应用注册一个名为“模型槽”的资源，绕过设备的模型并发上限 | M1 已实现（可信配置入口复制描述，启动后冻结，健康不改变持久状态）：`ResourceApi.registration_copies_trusted_metadata_and_health_never_commits_storage`、`ResourceApi.disabled_tables_and_invalid_descriptors_fail_explicitly`、`ResourceApi.configured_capacities_are_bounded_and_arena_size_accounts_for_both_tables`；v0.x 同进程原生代码为可信代码，不是隔离沙箱 |
| C-BUDGET-001 | 已废止（ADR-0042，取消累计费用预算）。历史规则：预算按设备 → 实例 → 目标 → 任务分层；子预算不得大于父级，各级上限同时成立 | 实例子预算之和超过设备总额，所有实例都在各自额度内但设备总额被突破 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-002 | 已废止（ADR-0042，取消累计费用预算）。历史规则：有成本的调用在准入时预留、在结果应用时结算；预留与 Task 创建同批提交（批次 B2），结算与结果应用同批提交（批次 B4） | 重启后预留丢失但 Task 仍在，任务再次准入时重复预留 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-003 | 已废止（ADR-0042，取消累计费用预算）。历史规则：重启不清零已花额度；结果未知或费用未知的调用按保守规则结算（按预留额计为已花费），经权威对账后再修正 | 重启后预算周期被重置，模型调用次数从 0 重新计算 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-004 | 已废止（ADR-0042，取消累计费用预算）。历史规则：模型与策略不能提高任何预算；预算只能由所有者的已认证管理命令或系统配置修改 | 模型回复“我需要更多额度”，策略据此提交提高预算的意图并被接受 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-005 | 已废止（ADR-0042，取消累计费用预算）。历史规则：超额时暂停分析或延后工作并说明原因，不自动改用产生额外费用的服务；超出已批准费用或数据目的地的 fallback 必须重新授权 | 主网关额度耗尽后自动切到按量计费的备用服务 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-006 | 已废止（ADR-0042，取消累计费用预算）。历史规则：系统保留额度（停止、诊断、前台交互）不能被普通工作占用；普通工作耗尽其预算时，保留部分仍可用 | 后台整理耗尽额度后，用户按键提问因预算不足被拒绝 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-007 | 已废止（ADR-0042，取消累计费用预算）。历史规则：余额不足时拒绝（`AYNI_BUDGET_EXHAUSTED`）、延后或暂停，并报告预算原因；不得先执行再补记超支 | 额度不足时仍发出模型请求，结算时把余额记为负数 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-008 | 已废止（ADR-0042，取消累计费用预算）。历史规则：一个实例耗尽其预算不影响其他实例在设备总额内的合法工作 | A 的额度耗尽后，B 的合法请求也被拒绝 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-009 | 已废止（ADR-0042，取消累计费用预算）。历史规则：同一次调用只结算一次；取消或删除后到达的结果仍如实结算已发生的费用，但不重新开放已取消的工作 | 删除记忆后，已发出的整理请求返回，费用既没有记录也没有拒绝 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |
| C-BUDGET-010 | 已废止（ADR-0042，取消累计费用预算）。历史规则：网关同步执行外部费用限制；网关限制不替代 Core 的本地预算，两者任一不满足即拒绝 | 只依赖网关限额，网关故障放行时设备无限发请求 | 历史设计要求，不再实施或以旧实验验收；新范围见 C-RUN-001～008 |

## MA-018A 资源机制边界（ABI 0.29）

`ayni/resources.h` 提供可信注册、owner 线程健康报告与复制查询。注册时健康为 OFFLINE；CONFIGURING/RUNNING 可报告健康，RECOVERING/DRAINING/STOPPED 不接受修改。重新在线不解除占用或隔离。运行态不写持久记录；Capability/Provider 仍未接入该队列。

内部请求一次至多 8 个按目录索引递增的不同资源，资源组全部可用才占用；重复 ID/资源、超出容量、非法等待策略均在修改前拒绝。请求槽包括排队与持有中的请求，满时明确拒绝，无淘汰。每优先级按 instance_id 有序环轮转，同实例按入队 ticket FIFO；期限到达先向工作属主报告 pause/skip，只有属主显式移除后才推进，不静默丢弃。无界等待显式使用 UINT64_MAX，有限截止溢出夹到最后有限时刻。

每次入队有不回绕的 ticket，每次占用输出 `(call_id, generation=ticket)` claim；释放及隔离解除须精确匹配。调用 ID 重用不能接受前一占用的旧停止确认。此机制不验证物理停止证据本身，证据的授权/持久消费由后续调用/许可链承担。最坏选择为配置容量的平方扫描；接入 step 前须量测时延，当前不宣称 ESP32 时限已通过。
