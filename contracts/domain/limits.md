# 容量与限额

- **关联**：计划 §2.7、§3.2「容量」、§3.4「消息限额」、§4.2、§5.3；ADR-0014、ADR-0015、ADR-0016、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0021、ADR-0022、ADR-0026、ADR-0031；`MA-003`、`MA-004`、`MA-005`、`MA-010`、`MA-018`；E1 步骤 10，第 10.6 节「事件与容量」
- **属主**：`kernel`（配置校验与分发）；各限额由其对象的属主模块执行（如 Task 数由 `work/task`，预留完成槽由 `kernel`）

同一 ingress 实例累计接受的本地输入以 u64 排序，初始 0，每次成功入队才递增；达到 UINT64_MAX 后，ABI 0.25 的 `ayni_post_event` 返回 `CAPACITY_EXCEEDED`，不入队、不 wake，也不回绕。自动存储重载保留实例身份和序号；进程重启创建新实例，序号不跨实例比较。恢复水位在同一入口锁内采样。队列满的 `BUSY` 仍表示暂时背压，不能把序号耗尽当作可由 step 解除的背压（ADR-0038）。

## 初始值

下表全部是**初始配置假设，尚未实测**（计划 §4.2）。调整须有 Mock 压测或硬件测量证据，并记录 ADR（计划 §10.6 末段）。除注明外，每项都由 `ayni_config` 给出，Native 与 ESP32 可以取不同值，但语义相同。

| 项目 | 初始值 | 来源 | 耗尽时 |
|---|---:|---|---|
| Agent（`agent_capacity`，1–64；每个 Agent 一个驻留实例） | 4（计划要求 ≥ 2） | §4.2 | 注册返回 `CAPACITY_EXCEEDED` |
| 活跃与可恢复的 Task（`task_capacity`，1–256；结束的 Task 在表满时让位，最早结束的先让） | 16 | §4.2 | 没有已结束的 Task 可让位时不派生，occurrence 留在 Goal 上至多等待 GoalType 声明的上限，超限跳过并计数、记原因 `TASK_CAPACITY`；无界等待不跳过（ADR-0018、ADR-0020）。有停下的 Task 的 Goal 同样等待，原因 `TASK_HELD`，所以一个 Goal 停下的 Task 不随 occurrence 增加；停下的 Task 仍占位置，拒绝许多 Goal 的策略仍可能占满 Task 表（ADR-0022） |
| Goal（`goal_capacity`，1–1024，终态 Goal 也占用；终态 Goal 在表满时让位，最早结束的先让） | 32；M1 压测后复核 | §4.2 | 让位的 Goal 连同它已结束的 Task 移除，之后按旧 ID 返回 `NOT_FOUND`；没有终态 Goal 可让位时新 Goal 返回 `CAPACITY_EXCEEDED`（`GOAL_CAPACITY`）（ADR-0025） |
| 每 Goal 授权需求（`goal_requirement_capacity`，1～16；Since 0.20） | 4；显式 0 拒绝（ADR-0053 起不再有省略字段的短配置） | ADR-0037 | arena 的每 Goal 固定需求数；域/Scope 各用既有字节限额。超出配置拒绝，不部分发布；恢复超出配置报告 `STATE_EXCEEDS_CAPACITY`，不当作记录损坏。公开需求导出随 E2b |
| Goal 参数（`goal_parameter_max_bytes`，0–4096） | 128 字节 | ADR-0017 | 超出返回 `INVALID_ARGUMENT`（`GOAL_PARAMETERS`） |
| 子目标深度（`goal_depth_max`，0–8）/ 每目标未终结的子目标数（`goal_children_max`，0–64） | 2 / 4 | §5.3 | 拒绝创建，返回 `CAPACITY_EXCEEDED`（`GOAL_DEPTH`、`GOAL_CHILDREN`）；0 表示不允许子目标 |
| GoalType 版本（`goal_type_capacity`，1–64） | 8 | ADR-0017 | 注册返回 `CAPACITY_EXCEEDED` |
| 同时打开的会话（`session_capacity`，1–64） | 4 | ADR-0017 | `ayni_open_session` 返回 `CAPACITY_EXCEEDED` |
| 策略（`strategy_capacity`，1–64；每个 ID 一个版本） | 4 | ADR-0022 | 注册返回 `CAPACITY_EXCEEDED` |
| 一次决定的意图（`policy_intent_capacity`，1–64；每个策略另声明不超过它的 `max_intents_per_call`） | 8 | §4.2、ADR-0022 | 整次决定被拒绝，Task 停下并记原因 `POLICY_OUTPUT_LIMIT`，不截断（C-POLICY-002） |
| 每个 Task 的步骤（`task_step_capacity`，1–64；步骤唯一、只增不删） | 8 | ADR-0024 | 整次决定被拒绝，Task 停下并记原因 `STEP_CAPACITY`；所以一个 Task 至多被应用 `task_step_capacity` + 1 个决定 |
| 每个 Task 的私有检查点（`task_checkpoint_max_bytes`，0–4096；每个策略另声明不超过它的 `max_checkpoint_bytes`） | 64 字节 | ADR-0024 | 整次决定被拒绝，Task 停下并记原因 `POLICY_OUTPUT_LIMIT`，不截断（C-POLICY-002）；注册时超过上限返回 `INVALID_ARGUMENT` |
| 周期触发的最短周期（`AYNI_TRIGGER_PERIOD_MIN_US`，不可配置） | 1 s | ADR-0019 | 更短的周期返回 `INVALID_ARGUMENT`；occurrence 编号用尽时触发结束，不回绕 |
| Kernel 分配的 ID（每次启动，不可配置） | 2^32−1 | C-ID-010 | 新 Goal 与会话返回 `CAPACITY_EXCEEDED`（`ID_EXHAUSTED`）；Task 不派生，occurrence 等待至 GoalType 的上限后以 `ID_EXHAUSTED` 跳过（ADR-0020） |
| 普通事件入口（`ingress_capacity`，1–4096） | 128 项 | §4.2 | 可丢弃事件覆盖同一订阅、同一非零合并键的最新一条，否则逐出最旧的可丢弃事件；其他返回 `BUSY`（ADR-0015） |
| 单个事件正文（`event_body_max_bytes`，1–4096） | 128 字节 | ADR-0015 | 超出返回 `INVALID_ARGUMENT` |
| 预留控制容量（完成槽，`completion_slot_count`，1–1024） | 16，且不小于在途请求上限 | §4.2 | 不调用端口，返回 `BUSY`（C-HOST-006） |
| 存储缓冲（`storage_buffer_bytes`，0 或 256–1048576，且不小于 W + 4；W为当前记录与容量的保守批次上界，计入min(需求容量,Grant容量)条Grant创建/撤销根及progress/完整命令，不要求各类记录同时发生） | 0：3×max(W+4,256)，与query/create共用具名容量公式 | ADR-0016、ADR-0026 | 恢复时是读取缓冲，Host 报告完整批次所需长度后以 `STATE_EXCEEDS_CAPACITY` 进入 `RESTRICTED`；运行中容纳尚未存储的批次，没有放下 W 的空间时意图返回 `BUSY`（`STORAGE_BACKLOG`），到期工作、决策与事件等待追加完成 |
| 每个 principal 保留的命令映射（`command_capacity`，1–1024） | 32 | ADR-0026 | 未pin且`command_id`最小的映射让位并提高下限；全pin普通新命令BUSY/COMMAND_PINNED；已接受撤销仍先查pending；未映射且不大于下限的命令返回 `EXPIRED`（`COMMAND_EXPIRED`） |
| 资源目录（`resource_capacity`，0–64；Since 0.29） | 0，显式 profile 启用 | ADR-0042 | 注册满时 `CAPACITY_EXCEEDED`，不自动发现或替换 |
| Device / Component 目录（Since 0.32） | 默认各 0；上限分别 64 / 256 | ADR-0046 | 满时明确拒绝；每个 Resource 至多关联 8 个唯一 Component；全部复制到 caller arena |
| 排队/持有资源组（`resource_request_capacity`，0–256；Since 0.29） | 0，显式 profile 启用；每组至多 8 个不同资源 | ADR-0042 | 明确拒绝，不淘汰已有请求；ABI 0.31 调用已消费该队列 |
| 活跃外部推理槽 | 1 | §4.2 | 排队（C-RES-004） |
| 每个 Task 的模型决策次数 | 8 | §4.2 | Task 暂停并报告原因 |
| 单轮能力请求数 | 8 | §2.7、§4.2 | 整轮拒绝 |
| 模型请求超时 | 30 s | §4.2 | 调用超时，按能力属性处理 |
| Capability（`capability_capacity`，1–256，按 ID+版本计数） | 16 | ADR-0028、ADR-0029 | 注册返回 `CAPACITY_EXCEEDED`；`CapabilityApi.registration_is_bounded_copies_identity_and_never_commits_or_wakes`、`CapabilityRegistry.filling_the_registry_preserves_every_prior_domain_and_lookup` |
| 单个授权 Scope 与完整注册域（`authority_scope_max_bytes`，64–4096 B） | 256 B | ADR-0029 | 完整规范表达超过字节容量拒绝，不截断；默认不保证容纳所有最大字段/成员组合。`CapabilityRegistry.complete_domain_fits_exactly_or_rejects_one_byte_more_without_partial_registration`、`CapabilityRegistry.largest_field_and_member_counts_fit_the_explicit_maximum_byte_configuration` |
| 每能力授权字段 / 每字段集合原始输入成员（不可配置） | 8 / 8 | ADR-0028、ADR-0029 | 超限拒绝；先检查原始 count 再排序去重，不用重复项绕过输入工作量上限。`CapabilityRegistry.token_count_is_bounded_before_deduplication_or_member_access`、`CapabilityRegistry.invalid_top_level_descriptors_never_take_a_slot` |
| Grant（`grant_capacity`，1–1024） | 32 | ADR-0030 | 仅最早且终态持久的安全叶节点可让位，无候选返回 `CAPACITY_EXCEEDED/GRANT_CAPACITY`；C 已实现并完成 Native 验证，D Gate/cleanup 安全条件见 ADR-0031 |
| 委派深度（`grant_delegation_depth_max`，1–16，按边数根0） | 4 | ADR-0030 | `CAPACITY_EXCEEDED/GRANT_DEPTH`；该配置限制遍历/合法链，不新增每节点子数组 |
| 每条auth命令完整规范请求 | 创建84 B固定头上界+`authority_scope_max_bytes`；撤销固定16 B；批准请求156 B固定头/ID上界+`goal_parameter_max_bytes`，结果52+12×`goal_requirement_capacity` B；stride取创建请求与批准请求+结果的较大值 | ADR-0030、0031 | CommandTable逐槽字节stride及有界scratch，超限明确拒绝；FNV仅索引，不降成摘要比较 |
| 撤销 Gate / 专用 pending（随 grant_capacity，无新配置） | 各32项；最多每target一项 | ADR-0031 | 各owner从caller arena有界分配，超限 `CAPACITY_EXCEEDED/REVOCATION_CAPACITY`；拒绝不装半Gate、不占命令ID；Native caller arena 已两轮严格C99量测，见下文；ESP32资源不据此推定 |
| Resource、订阅、排队长度 | 可配置 | §4.2 | `CAPACITY_EXCEEDED`，各有耗尽测试 |
| 控制帧 | 8 KiB，嵌套深度 8 | §3.4 | 拒绝（C-JSON-001、C-JSON-002） |
| 媒体缓冲、上下文 | 独立缓冲池与字节上限 | §4.2 | 拒绝新采集或缩小上下文 |
| 日志与快照 | 每槽最大快照按工作容量计算；日志默认 `max(1 MiB,8×buffer)`，75% 或侵入维护预留时轮转；无额外历史档案 | ADR-0027、§4.2 | 空间不足 `STORAGE_FAILED/STORAGE_SPACE`，不丢当前有效来源（C-COMMIT-015） |
| 持久请求间隔（`storage_write_interval_us`，0 或 1–1000000） | 0：10000 μs；追加、重试、栅栏、快照与回收共用，读不限 | ADR-0027 | 延后到 `next_deadline_us`，缓冲满时 `BUSY`，不跳过提交与屏障 |
| 日志编码字节上限（`storage_log_max_bytes`，0 或有足够维护预留且≤64 MiB） | 0：自动值 | ADR-0027 | 无法保证回收后前进的配置 `INVALID_ARGUMENT`；旧源允许流式迁移后收敛 |
| 派生摘要 lineage | 有界，M1 确定 | §5.2 | 缩小输入或拒绝（C-PROD-032） |

## 测量方法

计划 §4.2 要求 M0 定义以下指标的测量方法；数值在 M1（Native）与 M2（ESP32）测得，运动响应在 M3 测得。

| 指标 | 定义 | 测量方法 |
|---|---|---|
| 单次 `step` 时长 | 从 `ayni_step` 进入到返回的时间 | Host 单调时钟在调用前后采样；记录最大值、p99 与对应的 `max_items`；ESP32 另用 GPIO 翻转 + 逻辑分析仪复核 |
| 本地事件处理延迟 | 从 `ayni_post_event` 返回到对应 transition 应用完成 | 事件携带 Host 采样的入队时刻，trace 记录应用时刻；以 `step` 次数和单调时间两种单位报告 |
| 资源最长等待 | 请求进入队列到准入或超限处理 | 资源模块按请求记录，E1 步骤 3 断言 |
| 停止响应上限 | 停止触发到使能脚失能、到实际停止 | 按 `permits_and_stop.md` 的故障类型分项测量（C-STOP-008、C-STOP-009） |
| 内存基线 | 内部堆、DMA 堆、PSRAM 的总量/空闲/最小空闲/最大连续块，任务栈水位 | M0 Hello 固件已在启动时输出（`hosts/esp_idf` 的 `log_platform_baseline`）；每份报告保存编译选项与固件大小 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-LIMIT-001 | 全部容量由配置在创建时给出；Core 不无界增长，达到上限返回 `AYNI_CAPACITY_EXCEEDED`（或文档规定的 `BUSY`），不得静默丢弃持久工作 | 事件队列满时最旧的存储确认被覆盖 | M1 已实现（入口、完成槽、存储缓冲与工作模型）：`KernelLifecycle.batch_larger_than_the_buffer_reports_capacity`、`IngressQueue.ten_thousand_events_pass_in_order_without_loss`、`KernelApi.capacity_outside_its_range_is_invalid_argument`、`KernelApi.memory_requirements_grow_with_each_capacity`、`KernelApi.goal_tree_limits_need_no_memory`、`WorkModel.full_goal_table_refuses_without_side_effects`、`WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`；超过等待上限的 occurrence 带计数与原因跳过，不是静默丢弃（ADR-0020）：`WorkWait.occurrences_are_skipped_once_their_wait_ends`；恢复时本次容量放不下日志中的状态，以 `STATE_EXCEEDS_CAPACITY` 受限而不丢弃其中的工作（ADR-0026 决策 5）：`WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`、`GoalRestore.restore_reports_what_this_boots_limits_cannot_hold`；其余容量随各自工作包；`MA-003`；第 10.6 节 ；M1 部分实现（Goal 需求 arena/恢复容量）：`KernelApi.goal_requirement_capacity_must_be_in_its_range`、`GoalAuthorityPersistence.smaller_requirement_capacity_classifies_journal_and_snapshot_as_no_room`；ADR-0037 |
| C-LIMIT-002 | 每项限额都有耗尽测试，断言耗尽时的返回值、状态不变与恢复路径 | Grant 上限只在文档中出现，没有任何测试触及 | M1 已实现（已有的限额）：入口 `IngressQueue.full_queue_rejects_other_events_with_busy`；完成槽 `CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`；存储缓冲 `KernelLifecycle.batch_larger_than_the_buffer_reports_capacity`；主体、Agent 与 GoalType `WorkModel.registration_refuses_duplicates_and_capacity`；策略 `WorkStrategy.registration_checks_its_arguments`；一次决定的意图 `WorkDecision.refused_decisions_hold_the_task_with_their_reason`；Goal `WorkModel.full_goal_table_refuses_without_side_effects`；Goal 参数 `WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`；子目标 `WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；会话 `WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`；ID `WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`、`WorkWait.used_up_ids_skip_with_their_reason`；Task `WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`TaskTable.full_table_of_live_tasks_refuses_and_changes_nothing`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`；其余限额随各自工作包；`MA-018`；`MA-005` |
| C-LIMIT-003 | 初始值是未实测的工程假设；修改须附测量证据并记录 ADR | 压测失败后把 Task 上限从 16 改为 4，没有记录原因 | 评审；`reports/core/` |
| C-LIMIT-004 | Core 的存储全部来自调用方提供的内存与配置声明的容量；Core 不调用堆分配（ADR-0014） | Core 在注册 Capability 时调用 `new` 扩展数组 | M0 已实现：ctest `core_does_not_reference_heap_allocator` 检查每个 Core 静态库不引用 `malloc`/`free`/`operator new`/`operator delete`，也不引用 `strdup`、`asprintf`、`getline` 等替调用方分配内存的 libc 函数；ctest `heap_check_detects_allocator` 与 ctest `heap_check_detects_strdup` 证明该检查有效 |
| C-LIMIT-005 | 每个 Task 的模型决策次数达到上限时，Task 暂停并报告原因，不继续请求模型 | Tool Loop 在模型反复返回同一工具调用时无限循环 | `MA-009`；E1 步骤 5 |
| C-LIMIT-006 | 单轮最多 8 个能力调用是受理上限；超出时整轮拒绝并报告原因，不截断后执行前 8 个 | 策略返回 12 个调用，Kernel 默默执行前 8 个，后 4 个丢失 | E4 步骤 3；`MA-009`；C-CALL-006 |
| C-LIMIT-007 | 子目标深度与数量有界；周期触发在重启后不一次性补建无限 Task，同一触发窗口去重 | 断电一周后重启，每小时一次的检查一次性生成 168 个 Task | M1 已实现（子目标深度与数量）：`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.tree_limits_refuse_deeper_or_more_children`、`GoalTable.zero_limits_allow_top_level_goals_only`、`WorkModel.tree_limits_at_their_defaults_bound_the_tree`，子目标排在父目标之前时同样（ADR-0025）`GoalTable.child_in_an_earlier_place_is_still_in_its_parents_tree`；M1 已实现（本次启动内的去重）：等待的 occurrence 合并为一个 Task，节拍不变（ADR-0019）`TriggerSchedule.waiting_occurrences_are_taken_together_and_the_schedule_holds`、`WorkTrigger.occurrences_missed_between_steps_become_one_task`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`；M1 已实现（重启后：不补建断电期间错过的 occurrence，计时从恢复时刻按完整时长重新开始，ADR-0026 决策 6）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`GoalRestore.reanchor_restarts_every_wait_in_full_at_recovery`；日历提醒的“错过补发一次”随 `MA-019`（C-PROD-018）；E2 F8 |
| C-LIMIT-008 | 普通事件入口有界；遥测按声明合并或丢旧并报告 gap；存储确认、终态、撤销与资源回收使用预留控制容量；停止不经队列（C-STOP-001） | 摄像头遥测占满队列，撤销事件被丢弃 | M1 已实现（入口与完成槽）：`IngressQueue.full_queue_rejects_other_events_with_busy`、`IngressQueue.full_queue_replaces_newest_value_of_the_same_stream`、`IngressQueue.telemetry_flood_keeps_each_stream_in_order_and_accounted`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；本地输入不丢弃、队列满时返回 `BUSY`（ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`WorkInput.small_event_bodies_still_hold_a_source`；持续的事件流不挡到期工作：`WorkInput.steady_events_do_not_hold_back_due_work`；E1 步骤 10；`MA-003` |
| C-LIMIT-009 | 媒体缓冲与上下文使用独立的缓冲池与字节上限，不挤占关键任务栈与内部 RAM | 一张大图片占满内部 RAM，停止监督任务栈溢出 | M2/M3 资源测量；E1 步骤 8 |
| C-LIMIT-010 | 分配失败、存储满、TLS 失败都返回明确状态；拒绝新负载，同时保留停止与诊断能力 | 存储满时 Kernel 崩溃重启，停止按键失效 | 第 10.6 节「存储与恢复」；`MA-012` |
| C-LIMIT-011 | 一次 `ayni_step` 最多处理 `max_items` 个工作项，并在 `max_time_us` 用尽后返回；`step` 内不等待网络、存储、模型或设备 | 一次 `step` 内同步等待 Flash 写完 | M1 已实现：`KernelLifecycle.step_applies_at_most_max_items`、`KernelLifecycle.step_returns_once_its_time_budget_is_spent`、`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`（请求在之后的 step 中完成）、`KernelLifecycle.recovery_waits_for_a_free_slot_without_spinning`（等待空闲槽时不报告 `more_ready`）；策略的决定每个算一个 item，只取本次 step 之前已是 `READY` 的 Task（ADR-0022）：`WorkDecision.step_stops_deciding_once_its_time_budget_is_spent`、`WorkDecision.task_is_decided_in_the_step_after_it_became_ready`；`MA-003`；第 10.6 节「事件与容量」 |
| C-LIMIT-012 | 派生摘要的 lineage 条目数有上限，上限随配置给出；达到上限时按 C-PROD-032 处理 | lineage 数组写满后静默丢弃最早的依赖 | M1 已实现：`MemoryFacts.dependency_union_is_atomic_and_never_truncates`；C-MEM-005 |

D 的 Grant metadata 新增 revoker/cursor/独立 cleanup_stamp，snapshot Grant 尾为80 B，v3撤销mapping为80 B。Gate 在 GrantTable，pending metadata 与canonical/window scratch在CommandTable，按其 storage_size 统一分配；不在Kernel增加状态数组，也不另建普通结果窗口。清理每工作项至多处理一个候选，有限表/祖先扫描受配置界限约束；Native caller arena已两轮严格C99量测：默认110,792 B、最小31,616 B、对齐8 B，较C增加4,272/544 B。Gate/pending耗尽、全pin、背压与持久安全叶回收已有Native场景证据；容量失败组合未逐项穷尽，见ADR-0031。硬件RAM、栈与最坏step时长仍待量测，结构上界或Native arena数字不替代硬件证据。

## ABI 0.31 调用配置

`call_capability_capacity`、`provider_capacity`、`binding_capacity`、`call_capacity` 缺省或显式 0 均为关闭；原基础/Hello 配置不自动分配全部调用表。启用调用时，输入 canonical stride 默认 4096 B、最多 131072 B，结果默认 4096 B、最多 65536 B；每个契约的正文上限还须在对应 stride 内。CallTable、ProviderMailbox、CallRuntime staging/receipt 与原域字节由同一 caller arena 预算，创建后不扩容。Call 槽上限 256，保留 UNKNOWN 也占槽，不能自动淘汰。

每个 Task 的步骤上限计算本地完成步骤和已接受调用 step_key 的并集。失败调用不释放该身份；模型次数另由 Task owner 保存。管理查询对账只占独立有界 Provider 完成槽，不重新占用原执行资源或模型调用次数。

## ABI 0.33 工作频率

后台 GoalType 只接受 INTERVAL，默认最短 300,000,000 μs，可信配置可设有限值且不得小于 AYNI_TRIGGER_PERIOD_MIN_US；非后台类型的频率字段须为 0。后台派生前 occurrence 等待必须有限且不超过 30,000,000 μs；提醒必须无界等待。Goal 请求小于该下限时返回 INVALID_ARGUMENT/BACKGROUND_FREQUENCY。优先级及等待上限由可信分类决定，不由模型提高；这是每个 Goal 的运行频率约束，不是 Agent 累积费用或调用额度。

## P3 记忆与来源容量（ABI 0.40）

`memory_capacity` 默认 64、0 禁用、上限 256；`memory_body_max_bytes` 默认 512、范围 1～4096；`memory_dependency_capacity` 默认 8、范围 1～8。每个 Memory/Goal/Task/Context/命令来源集合都受同一依赖上限约束，不能把整张图压成一个 anchor 规避上限。Context 另占每 Agent 一个有界槽；清理期间保守保留 Goal/Task，可能暂时阻止回收。表满拒绝新记录，见 C-MEM-002/005。

Native 对话正常保留最近 3 轮；第 4 槽只承接发布失效时已接受的独立用户输入，满 4 槽须先显式删除一轮才能提交新输入。未发布草稿最多 384 UTF-8 字节；续写合并为同一未编号输入，超限拒绝且保留原值。摘要最多 512 字节、portable state 4096 字节，两个限制分别检查。

## P4 观测与 Mock 媒体容量（ABI 0.42）

source 容量为 0 时样本 FIFO 默认关闭，显式启用 FIFO 而无 source 返回 INVALID_ARGUMENT；有 source 时 observation_ingress_capacity 的 0 选择默认 16，显式范围 1～256。gap 独占每源一个槽，上限随 source 的 64，不消耗样本 FIFO。Core 只复制 metadata，不分配帧字节。Mock MediaStore 默认 4 对象 × 4096 B，可配置 1～64 对象、每对象 1～65536 B；分配/对象/字节耗尽显式 CAPACITY_EXCEEDED，不覆盖现有对象。见 C-OBS-002/006。

ABI 0.43 / ADR-0063：订阅复用 call_capacity（最多256）和资源队列容量，不新增无限表；每 Call 一个 scoped Observation 槽、worker gate 与可靠 gap、一个独立 lease ACK 元数据槽。FIFO 同 Host 共用 observation_ingress_capacity，满返回 BUSY；gaps_pending 上限为 source_capacity + call_capacity。默认 lease 30000 ms，可信契约可覆盖为正 u32 毫秒，有限截止溢出拒绝。每 Task 每源最多一个 desiredOpen 订阅，显式关闭后可换新 step；忙时只保留最新知识，不积压分析任务。等待前预留 wait/wake/final-cancel 三次版本变更。
