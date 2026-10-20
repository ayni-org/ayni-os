# 容量与限额

- **关联**：计划 §2.7、§3.2「容量」、§3.4「消息限额」、§4.2、§5.3；ADR-0014、ADR-0015、ADR-0016、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0021、ADR-0022；`MA-003`、`MA-004`、`MA-005`、`MA-018`；E1 步骤 10，第 10.6 节「事件与容量」
- **属主**：`kernel`（配置校验与分发）；各限额由其对象的属主模块执行（如 Task 数由 `work/task`，预留完成槽由 `kernel`）

## 初始值

下表全部是**初始配置假设，尚未实测**（计划 §4.2）。调整须有 Mock 压测或硬件测量证据，并记录 ADR（计划 §10.6 末段）。除注明外，每项都由 `ayni_config` 给出，Native 与 ESP32 可以取不同值，但语义相同。

| 项目 | 初始值 | 来源 | 耗尽时 |
|---|---:|---|---|
| Agent（`agent_capacity`，1–64；每个 Agent 一个驻留实例） | 4（计划要求 ≥ 2） | §4.2 | 注册返回 `CAPACITY_EXCEEDED` |
| 活跃与可恢复的 Task（`task_capacity`，1–256；结束的 Task 在表满时让位，最早结束的先让） | 16 | §4.2 | 没有已结束的 Task 可让位时不派生，occurrence 留在 Goal 上至多等待 GoalType 声明的上限，超限跳过并计数、记原因 `TASK_CAPACITY`；无界等待不跳过（ADR-0018、ADR-0020）。有停下的 Task 的 Goal 同样等待，原因 `TASK_HELD`，所以一个 Goal 停下的 Task 不随 occurrence 增加；停下的 Task 仍占位置，拒绝许多 Goal 的策略仍可能占满 Task 表（ADR-0022） |
| Goal（`goal_capacity`，1–1024，终态 Goal 也占用） | 32；M1 压测后复核 | §4.2 | 新 Goal 返回 `CAPACITY_EXCEEDED`（`GOAL_CAPACITY`）；终态 Goal 的回收随 `MA-010` |
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
| 存储缓冲（`storage_buffer_bytes`，256–65536） | 1024 字节 | ADR-0016 | 单个批次的上限；恢复读到放不下的批次时以 `STORAGE_READ_FAILED` 进入 `RESTRICTED` |
| 活跃外部推理槽 | 1 | §4.2 | 排队（C-RES-004） |
| 每个 Task 的模型决策次数 | 8 | §4.2 | Task 暂停并报告原因 |
| 单轮能力请求数 | 8 | §2.7、§4.2 | 整轮拒绝 |
| 模型请求超时 | 30 s | §4.2 | 调用超时，按能力属性处理 |
| Capability | 32 | §4.2 | 注册返回 `CAPACITY_EXCEEDED` |
| Resource、Grant、订阅、排队长度 | 可配置 | §4.2 | `CAPACITY_EXCEEDED`，各有耗尽测试 |
| 控制帧 | 8 KiB，嵌套深度 8 | §3.4 | 拒绝（C-JSON-001、C-JSON-002） |
| 媒体缓冲、上下文 | 独立缓冲池与字节上限 | §4.2 | 拒绝新采集或缩小上下文 |
| 日志与快照 | 轮转大小、快照/历史大小、写频率有上限 | §4.2 | 新副作用返回 `STORAGE_FAILED`（C-COMMIT-015） |
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
| C-LIMIT-001 | 全部容量由配置在创建时给出；Core 不无界增长，达到上限返回 `AYNI_CAPACITY_EXCEEDED`（或文档规定的 `BUSY`），不得静默丢弃持久工作 | 事件队列满时最旧的存储确认被覆盖 | M1 已实现（入口、完成槽、存储缓冲与工作模型）：`KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`、`IngressQueue.ten_thousand_events_pass_in_order_without_loss`、`KernelApi.capacity_outside_its_range_is_invalid_argument`、`KernelApi.memory_requirements_grow_with_each_capacity`、`KernelApi.goal_tree_limits_need_no_memory`、`WorkModel.full_goal_table_refuses_without_side_effects`、`WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`；超过等待上限的 occurrence 带计数与原因跳过，不是静默丢弃（ADR-0020）：`WorkWait.occurrences_are_skipped_once_their_wait_ends`；其余容量随各自工作包；`MA-003`；第 10.6 节 |
| C-LIMIT-002 | 每项限额都有耗尽测试，断言耗尽时的返回值、状态不变与恢复路径 | Grant 上限只在文档中出现，没有任何测试触及 | M1 已实现（已有的限额）：入口 `IngressQueue.full_queue_rejects_other_events_with_busy`；完成槽 `CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`；存储缓冲 `KernelLifecycle.batch_larger_than_the_buffer_restricts_as_read_failed`；主体、Agent 与 GoalType `WorkModel.registration_refuses_duplicates_and_capacity`；策略 `WorkStrategy.registration_checks_its_arguments`；一次决定的意图 `WorkDecision.refused_decisions_hold_the_task_with_their_reason`；Goal `WorkModel.full_goal_table_refuses_without_side_effects`；Goal 参数 `WorkModel.parameters_are_checked_last_by_size_and_by_the_goal_type`；子目标 `WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；会话 `WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`；ID `WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkTask.used_up_ids_leave_the_occurrence_waiting`、`WorkWait.used_up_ids_skip_with_their_reason`；Task `WorkTask.full_task_table_reclaims_the_task_that_ended_first`、`TaskTable.full_table_of_live_tasks_refuses_and_changes_nothing`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`；其余限额随各自工作包；`MA-018`；`MA-005` |
| C-LIMIT-003 | 初始值是未实测的工程假设；修改须附测量证据并记录 ADR | 压测失败后把 Task 上限从 16 改为 4，没有记录原因 | 评审；`reports/core/` |
| C-LIMIT-004 | Core 的存储全部来自调用方提供的内存与配置声明的容量；Core 不调用堆分配（ADR-0014） | Core 在注册 Capability 时调用 `new` 扩展数组 | M0 已实现：ctest `core_does_not_reference_heap_allocator` 检查每个 Core 静态库不引用 `malloc`/`free`/`operator new`/`operator delete`，也不引用 `strdup`、`asprintf`、`getline` 等替调用方分配内存的 libc 函数；ctest `heap_check_detects_allocator` 与 ctest `heap_check_detects_strdup` 证明该检查有效 |
| C-LIMIT-005 | 每个 Task 的模型决策次数达到上限时，Task 暂停并报告原因，不继续请求模型 | Tool Loop 在模型反复返回同一工具调用时无限循环 | `MA-009`；E1 步骤 5 |
| C-LIMIT-006 | 单轮最多 8 个能力调用是受理上限；超出时整轮拒绝并报告原因，不截断后执行前 8 个 | 策略返回 12 个调用，Kernel 默默执行前 8 个，后 4 个丢失 | E4 步骤 3；`MA-009`；C-CALL-006 |
| C-LIMIT-007 | 子目标深度与数量有界；周期触发在重启后不一次性补建无限 Task，同一触发窗口去重 | 断电一周后重启，每小时一次的检查一次性生成 168 个 Task | M1 已实现（子目标深度与数量）：`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`、`GoalTable.tree_limits_refuse_deeper_or_more_children`、`GoalTable.zero_limits_allow_top_level_goals_only`、`WorkModel.tree_limits_missing_from_the_config_take_their_defaults`；M1 已实现（本次启动内的去重）：等待的 occurrence 合并为一个 Task，节拍不变（ADR-0019）`TriggerSchedule.waiting_occurrences_are_taken_together_and_the_schedule_holds`、`WorkTrigger.occurrences_missed_between_steps_become_one_task`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`；重启后的补发随 `MA-010`；E2 F8 |
| C-LIMIT-008 | 普通事件入口有界；遥测按声明合并或丢旧并报告 gap；存储确认、终态、撤销与资源回收使用预留控制容量；停止不经队列（C-STOP-001） | 摄像头遥测占满队列，撤销事件被丢弃 | M1 已实现（入口与完成槽）：`IngressQueue.full_queue_rejects_other_events_with_busy`、`IngressQueue.full_queue_replaces_newest_value_of_the_same_stream`、`IngressQueue.telemetry_flood_keeps_each_stream_in_order_and_accounted`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；本地输入不丢弃、队列满时返回 `BUSY`（ADR-0021）：`KernelEvents.full_queue_refuses_until_a_step_takes_one`、`WorkInput.small_event_bodies_still_hold_a_source`；持续的事件流不挡到期工作：`WorkInput.steady_events_do_not_hold_back_due_work`；E1 步骤 10；`MA-003` |
| C-LIMIT-009 | 媒体缓冲与上下文使用独立的缓冲池与字节上限，不挤占关键任务栈与内部 RAM | 一张大图片占满内部 RAM，停止监督任务栈溢出 | M2/M3 资源测量；E1 步骤 8 |
| C-LIMIT-010 | 分配失败、存储满、TLS 失败都返回明确状态；拒绝新负载，同时保留停止与诊断能力 | 存储满时 Kernel 崩溃重启，停止按键失效 | 第 10.6 节「存储与恢复」；`MA-012` |
| C-LIMIT-011 | 一次 `ayni_step` 最多处理 `max_items` 个工作项，并在 `max_time_us` 用尽后返回；`step` 内不等待网络、存储、模型或设备 | 一次 `step` 内同步等待 Flash 写完 | M1 已实现：`KernelLifecycle.step_applies_at_most_max_items`、`KernelLifecycle.step_returns_once_its_time_budget_is_spent`、`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`（请求在之后的 step 中完成）、`KernelLifecycle.recovery_waits_for_a_free_slot_without_spinning`（等待空闲槽时不报告 `more_ready`）；策略的决定每个算一个 item，只取本次 step 之前已是 `READY` 的 Task（ADR-0022）：`WorkDecision.step_stops_deciding_once_its_time_budget_is_spent`、`WorkDecision.task_is_decided_in_the_step_after_it_became_ready`；`MA-003`；第 10.6 节「事件与容量」 |
| C-LIMIT-012 | 派生摘要的 lineage 条目数有上限，上限随配置给出；达到上限时按 C-PROD-032 处理 | lineage 数组写满后静默丢弃最早的依赖 | `MA-011` |
