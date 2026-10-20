# ADR-0022：工作模型第三部分（一）：策略与决策

- **状态**：已接受（`MA-004` 第三部分的第一个 PR 在 M1 已实现）
- **日期**：2026-10-20
- **关联**：计划 §2.4、§2.5、§2.7、§3.2「生命周期」「新策略」、§4.2、§5.3；`MA-004`、`MA-008`、`MA-009`、`MA-010`、`MA-018`；ADR-0006、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0021；C-POLICY-001、C-POLICY-002、C-POLICY-003、C-POLICY-004、C-POLICY-006、C-POLICY-009、C-POLICY-010、C-LIFE-003、C-LIFE-007、C-COMMIT-012、C-LIMIT-006、C-LIMIT-011、C-VER-003

## 背景

`MA-004` 第二部分让 Goal 按触发派生 Task，但 Task 派生后只能停在 `READY`，除取消与失效外不会结束：没有任何代码决定一个 Task 做什么。ADR-0021 的后果中也记下，Task 不会完成，周期触发与本地输入会占满 Task 表。ADR-0006 定义了策略接口的形态（有界决策函数，返回意图），但它仍在草案 `include/ayni/draft/policy_v1.h` 中。

第三部分原定包括策略与决策循环、步骤与检查点、策略意图、`blocked_on` 与 C-LIFE-024。其中 `blocked_on` 由待决调用派生，在途 Task 的暂停点与取消传播针对在途调用，而调用要到 `MA-008`、`MA-018` 才有；实例的 `STARTING`、`FAULTED` 与重启后的检查点兼容要等 `MA-010` 的持久化。没有在途调用时，`blocked_on` 只能恒为空，写了也无法验证。

其中三个问题已请用户决定（决策 1、7、8）；本 ADR 按推荐项实现，用户改选时修订本 ADR 与相应实现。（2026-10-20 用户确认了这些推荐项。）停下的 Task 是否阻止其 Goal 继续派生（决策 10）由用户选定“停止派生”。

## 决策

1. **按依赖拆分（推荐项）。** 第三部分分为两个 PR：本 PR 是策略与决策，下一个 PR 是步骤与检查点。`blocked_on`、在途请求的暂停点与取消传播移到 `MA-008`、`MA-018`；实例的 `STARTING`、`FAULTED` 与重启后的检查点兼容随 `MA-010`。本 PR 只实现结束 Task 的两种意图，不含私有检查点（C2）。（步骤与私有检查点已由 ADR-0024 实现。）
2. **策略注册（ABI 0.10）。** `ayni_register_strategy` 在 `CONFIGURING` 中登记 `ayni_strategy_v1`：目录 ID、非零版本、调用方持有的 `ctx`、`decide`、每次调用的意图上限 `max_intents_per_call`（1 到 `policy_intent_capacity`），以及策略编译时的 `sizeof(ayni_policy_intent_v1)` `intent_size`，即它写意图的步长。`intent_size` 必须等于本库的大小，规则与 `struct_size` 相同：较小为 `INVALID_ARGUMENT`，较大为 `UNSUPPORTED_VERSION`。意图数组由 Kernel 按自己的步长分配，以后的版本加长意图时，Kernel 由此知道一个策略按哪个步长写；0.x 的小版本必须一致，所以 0.10 只接受相等。同一 ID 同时只登记一个版本：GoalType 按 ID 指定策略，策略只在停机与重启之间替换（C-POLICY-010），两个版本并存时无法说清哪个推进 Task。容量为 `strategy_capacity`（默认 4，1–64）。检查顺序与其他注册相同：空指针与 `struct_size`，`intent_size`，各字段，回调内调用与状态，最后是重复的 ID（`INVALID_ARGUMENT`）与容量（`CAPACITY_EXCEEDED`）。
3. **GoalType 指定策略，`ayni_start` 解析。** `ayni_goal_type_v1` 追加 `strategy_id`。会产生 Task 的 GoalType（`trigger_kinds` 不只是 `NONE`）必须给出目录 ID，只接受 `NONE` 的必须为空，因为它的 Goal 没有 Task 可决定。注册时不查策略是否存在，所以注册顺序无关；`ayni_start` 一次解析全部 GoalType 的策略，缺少时返回 `AYNI_NOT_FOUND` 并保持 `CONFIGURING`（C-LIFE-003），补登记后可以再次调用。之后注册关闭，解析出的下标在 Kernel 的生命期内有效，`step` 不再按 ID 查找。
4. **快照与输出。** `decide(ctx, snapshot, output)` 收到 `ayni_task_snapshot_v1`：Task 的版本、ID、Goal、owner 实例、到期时间与代表的 occurrence，决定它的 step 开始的时刻 `now_us`，GoalType 的 ID 与版本，以及 GoalType 接受的 Goal 参数。字符串与参数是指向 Kernel 内存的视图，只在本次调用中有效（C-POLICY-004）。策略只从快照读时间，相同快照给出相同决定（C-POLICY-006）。输出 `ayni_policy_output_v1` 由 Kernel 提供：意图数组在 Kernel 内存中，`policy_intent_capacity` 项（默认 8，1–64，与计划 §4.2 的单轮能力请求数一致），每次调用前清零该策略可用的前 `max_intents_per_call` 项。Kernel 只读回 `intent_count` 与自己的数组，策略改写 `intents` 指针也不会让 Kernel 读别处的内存。
5. **决策在 step 中的位置与预算。** `step` 依次应用完成、推进恢复，在 `RUNNING` 中做到期工作、做决策，最后取事件（ADR-0021）。决策只取在本次到期工作开始之前已是 `READY` 的 Task，所以本次 step 派生的 Task 在下一次 step 决定，一次 step 的工作有限，与策略如何回答无关。`READY` 最久的 Task 先决定（按进入 `READY` 的 `commit_seq`，再按 `task_id`）；每个决定是一个 item 与一个 transition，之后检查时间预算（C-LIMIT-011）。只有 `RUNNING` 做决策：`DRAINING` 与 `RESTRICTED` 不接收新工作（ADR-0018），决策也是新工作。仍有 `READY` Task 等待决定时，`step` 报告 `more_ready`。
6. **M1 的意图种类。** 实现 `AYNI_POLICY_REPORT_COMPLETION`（5）与 `AYNI_POLICY_REPORT_FAILURE`（6），二者都结束 Task；`ayni_policy_intent_v1` 只有 `struct_size`、`kind` 与 `expected_revision`。其余种类（提出目标、请求调用、请求模型、报告进度、写入记忆）与 `step_key`、依赖版本、正文留在草案，随步骤与调用追加在结构末尾。因此一个决定恰好含一个结束意图。（自 ADR-0024 起，`REPORT_PROGRESS` 与 `step_key` 已实现，一个决定可以只报告进度而不结束 Task。）
7. **结束 Task 与 Goal（产品语义，推荐项“一次性失败，周期继续”）。** 结束意图使 Task 进入 `COMPLETED` 或 `FAILED`。若它的 Goal 此后没有其他未结束的 Task，也不会再有 occurrence（`ONCE`、`DELAY` 的唯一一次，或编号用尽的周期与本地输入触发），Goal 在同一 transition 中相应进入 `SATISFIED` 或 `FAILED`；周期与本地输入 Goal 不受一次失败影响，下次触发照常派生。Task 与 Goal 的版本各加 1。
8. **Goal 结束时取消子目标（产品语义，推荐项“一并取消”）。** Goal 因此结束时，它未终结的后代与这些后代的 Task 在同一 transition 中取消并记录 `cancel_revision`，与取消、失效的级联一致。何时完成由父目标的策略决定，它可以先等子目标；剩下的子目标已不需要，也不会出现父目标已结束、子目标还在运行的状态。被取消的 Task 不再被决定。
9. **拒绝的决定使 Task 停下。** 不结束 Task 的决定都使它从 `READY` 进入 `PAUSED`，`ayni_task_info_v1` 追加的 `hold_status` 与 `hold_reason` 记下第一项未通过的检查（C-POLICY-002、C-POLICY-003）。检查顺序：`decide` 返回失败时为该状态码，`CAPACITY_EXCEEDED` 配原因 `POLICY_OUTPUT_LIMIT`（28），其余配 `NONE`；返回值大于 `AYNI_OK`、不是状态码时为 `INVALID_ARGUMENT`、`NONE`，所以停下的 Task 的 `hold_status` 总是失败；`intent_count` 超过 `intent_capacity` 为 `CAPACITY_EXCEEDED`、`POLICY_OUTPUT_LIMIT`；返回 `OK` 却没有意图为 `INVALID_ARGUMENT`、`POLICY_NO_PROGRESS`（30），Task 没有可等的东西；之后逐个意图检查：`struct_size` 不符为 `INVALID_ARGUMENT`、`POLICY_INTENT`（29），未实现的种类为 `UNSUPPORTED`、`INTENT_UNSUPPORTED`，`expected_revision` 不是快照的版本为 `REVISION_CONFLICT`、`TASK_REVISION`，第二个结束意图为 `INVALID_ARGUMENT`、`POLICY_INTENT`。整次决定被拒绝，不截断后执行前几个（C-LIMIT-006）。
10. **停下的 Task 保持停下，其 Goal 不再派生（产品语义，用户选定“停止派生”）。** Goal 或实例的恢复不放行停下的 Task，Kernel 也不再请策略决定它：快照只有版本与时间不同，再问多半得到同一个拒绝，而一直等待决定的 Task 会让 `more_ready` 保持置位，所有者不停地 step。停下的 Task 等人处理：取消与失效照常结束它，原因保留可读。它仍是未结束的 Task，占用 Task 表的位置，一次性 Goal 也因此不结束。它结束之前，它的 Goal 不派生新 Task：occurrence 照常到期并等待，Goal 信息的 `wait_reason` 为 `TASK_HELD`（31，`INVALID_STATE`，只用于 Goal 信息），等待超过 GoalType 的 `occurrence_wait_max_us` 后跳过并以同一原因记入 `skip_reason`（ADR-0020）；`TASK_CANCEL` 取消它之后 Goal 照常派生（自 ADR-0023 起，不会再有 occurrence 的 Goal 随它的最后一个 Task 取消）。否则拒绝周期或本地输入 Goal 的策略会在每次 occurrence 留下一个停下的 Task，占满所有 Goal 共用的 Task 表，其他 Agent 的 Goal 也无法派生。`TASK_HELD` 先于 `TASK_CAPACITY` 与 `ID_EXHAUSTED`，因为它只与这个 Goal 有关。一个 Goal 可以有多个停下的 Task（第一次停下之前派生的都可能被拒绝），最后一个结束后才恢复派生。这样一个 Goal 停下的 Task 不再随 occurrence 增加，但停下的 Task 仍占位置：拒绝许多 Goal 的策略仍可能占满 Task 表。Task 表记着未结束的停下 Task 数；不为 0 时，每次查找到期工作先遍历一次 Task，记下有停下 Task 的 Goal（按 `AYNI_GOAL_CAPACITY_MAX` 位，128 字节，在栈上），再逐个看 Goal，所以代价是 Goal 数加 Task 数，而不是二者之积。在 x86 Release 构建上，1024 个 Goal、256 个停下的 Task 时，逐个 Goal 查看 Task 的做法每次查找约 85 µs，没有停下的 Task 时约 4 µs；改为先遍历一次后约 5 µs。ESP32 上未测。
11. **版本不回绕。** 每个意图的 `expected_revision` 是快照中 Task 的版本（C-COMMIT-012）。结束会使 Task、Goal、某个后代或后代的 Task 的版本越过 2^32−1 时，Task 以 `CAPACITY_EXCEEDED`、`REVISION_EXHAUSTED` 停下，停下只提升 Task 自身的版本；版本已是最大值的 Task 不被决定，也不计入 `more_ready`，否则连停下都无法应用。在 2^32−2 上停下的 Task 停在最大版本：`TASK_CANCEL` 与它的 Goal 的取消都因 `REVISION_EXHAUSTED` 被拒绝，失效被放弃，Goal 因决策 10 不再派生，occurrence 一直以 `TASK_HELD` 跳过。这需要同一个 Task 改变约 4×10^9 次，不另作处理。
12. **回调期间不改变 Kernel。** `decide` 与 GoalType 的校验函数同属 Kernel 调用的函数：回调期间，接受非 const Kernel 的所有者线程函数在参数检查之后返回 `AYNI_INVALID_STATE` 且不产生效果（C-POLICY-001）。所以策略看到的 Task 就是它的决定所应用的 Task，M1 中版本冲突只在策略填错版本时出现。
13. **`RUNNING` 留给在途请求。** Task 的 `READY` 表示等待决定；`RUNNING` 留给有在途请求、等待其结果的 Task（`MA-008`）。M1 的决定在同一 step 中应用，查询看不到 `RUNNING` 的 Task。
14. **内存。** 默认配置下 Kernel 的内存需求从 39,184 字节增至 40,024 字节（+840；查找到期工作时另用 128 字节栈）：4 个策略各 64 字节，8 个意图各 12 字节，8 个 GoalType 各增 40 字节（`strategy_id` 与解析出的下标），16 个 Task 各增 8 字节（停下的状态码与原因），Task 表的停下计数 8 字节（含对齐），其余为对齐。工作模型取最小容量时从 26,368 字节增至 26,532 字节。

决策 2 中每个 ID 一个版本与 `intent_size`、决策 4 的输出由 Kernel 提供、决策 5 的顺序与预算、决策 9、11–13 与原因码的分配为实施细则，未单独请用户决定。

## 后果

- Task 终于会结束：结束的 Task 在 Task 表满时让位（ADR-0018），ADR-0021 所说周期触发与本地输入占满 Task 表的问题在策略结束 Task 后缓解；一次性提醒派生、决定、完成，Goal 进入 `SATISFIED`，E1 的规则策略有了入口。
- 一次性 Goal 只通过决定结束。用 `TASK_CANCEL` 取消它唯一的 Task，或等待超限跳过它唯一的 occurrence 时，Goal 仍是 `ACTIVE`，却不会再有 Task（自 ADR-0018、ADR-0020 起的行为）。是否让 Goal 随之结束已另请用户决定；推荐项是取消时进入 `CANCELLED`、跳过时进入 `FAILED` 并记原因，在本 PR 之后单独实现。（ADR-0023 已按推荐项实现。）
- 停下的 Task 只能被取消或随失效结束，不会自动重试。策略的缺陷因此不会循环消耗 step，代价是这类 Task 需要用户或 Agent 处理；`hold_status` 与 `hold_reason` 说明原因。在此之前它的 Goal 不派生，occurrence 等待或按等待上限跳过并记 `TASK_HELD`；其他 Goal 只少了停下的 Task 占用的位置。
- 会产生 Task 的 GoalType 必须指定已注册的策略，否则 `ayni_start` 失败。0.9 的调用方须重新编译（C-VER-006）：`ayni_goal_type_v1`、`ayni_task_info_v1` 只接受 0.10 的大小；`ayni_config_v1` 仍接受旧布局，新容量取默认值。
- 时间预算在每个决定之后检查，一次耗时很长的 `decide` 仍会超出 `max_time_us`。回调的工作量上限按 C-POLICY-011 在 `MA-009` 测量并写入注册描述。
- 决定与它结束的 Task、Goal 仍只在内存中；重启后的处理与检查点兼容随 `MA-010`。
- 意图数组按 `policy_intent_capacity` 分配一份，所有策略共用：决定逐个进行，同一时刻只有一个决定在写。

## 备选方案

- **第三部分整体等 `MA-008`、`MA-018`**：`MA-004` 要更晚才算完成，Task 在此之前一直不会结束。未选，见决策 1。
- **任何 Task 失败都结束其 Goal**：一次失败的整点检查会让以后的检查全部停下。未选，见决策 7。
- **Goal 等子目标都结束后再结束**：Goal 要记下自己 Task 的结果并逐级向上检查，放行规则下子目标在父目标结束前也已停下。未选，见决策 8。
- **拒绝的决定使 Task 失败**：策略的一次缺陷会直接结束一次性 Goal，丢掉诊断与人工处理的机会，也与 C-POLICY-002 的“暂停并报告原因”不符。否决。
- **拒绝后在下一次 step 再问策略**：快照只有版本与时间不同，多半每个 step 重复一次拒绝，一直等待决定的 Task 也使 `more_ready` 不再清零。否决，见决策 10。
- **停下的 Task 不影响派生**：拒绝周期 Goal 的策略会在每次 occurrence 留下一个停下的 Task，占满共用的 Task 表，其他 Goal 也无法派生。用户否决，见决策 10。
- **每个 Goal 同时只有一个未结束的 Task**：同样限制占用，但改变所有 Goal 的派生与 occurrence 合并的时机，超出本 PR。未选，以后可以另议。
- **由 Kernel 的 `sizeof` 隐含意图步长**：以后加长意图时，按旧头文件编译的策略会按旧步长写入 Kernel 的数组。否决，见决策 2。
- **派生它的 step 中就做决定**：一次 step 的工作量会随策略的回答增长；下一次 step 决定只多一次 `more_ready` 唤醒。否决，见决策 5。
- **决定在事件之后**：持续的事件流会占满 item 预算，与 ADR-0021 否决事件先于到期工作的理由相同。否决。
- **策略提供输出缓冲**：Kernel 要校验策略给出的指针与长度，策略也要为最大输出预留内存。否决，见决策 4。
- **同一 ID 并存多个版本，由 GoalType 指定版本**：与 C-POLICY-010 的“只在重启时替换”冲突，同一 Task 的策略版本也要另行记录。未选；版本兼容随检查点（C2）与 `MA-010`。
- **注册时立即查找策略**：注册顺序会影响结果，与 C-LIFE-003 不符。否决。
- **M1 就让 Task 进入 `RUNNING`**：决定在同一 step 中应用，`RUNNING` 不可观察，只会多一次转换；有在途请求时它才有意义。未选，见决策 13。

## 验证

- `tests/unit/kernel_strategy_test.cpp`（`WorkStrategy.*`）：注册参数、`struct_size`、`intent_size`、上限与状态（`WorkStrategy.registration_checks_its_arguments`）；GoalType 恰在会产生 Task 时指定策略（`WorkStrategy.goal_type_names_a_strategy_exactly_when_its_goals_may_have_tasks`）；`ayni_start` 解析策略，缺少时返回 `NOT_FOUND` 并保持 `CONFIGURING`，补登记后可以启动（`WorkStrategy.start_resolves_strategy_ids_and_waits_for_a_missing_one`）。
- 同一文件（`WorkDecision.*`）：派生后的下一次 step 决定，快照与输出的内容，一个 transition 中结束 Task 与一次性 Goal（`WorkDecision.task_is_decided_in_the_step_after_it_became_ready`）；失败使一次性 Goal 失败，周期 Goal 继续（`WorkDecision.failure_fails_a_one_shot_goal_and_a_recurring_goal_goes_on`）；Goal 随最后一个 Task 结束（`WorkDecision.goal_ends_with_the_last_of_its_tasks`）；Goal 随完成或失败的 Task 结束时取消后代与其 Task（`WorkDecision.goal_end_cancels_its_live_descendants_and_their_tasks`）；十三种拒绝各以其状态码与原因停下 Task，且不再询问策略（`WorkDecision.refused_decisions_hold_the_task_with_their_reason`）；每次从清零的意图开始，Kernel 只读自己的数组（`WorkDecision.strategy_starts_from_cleared_intents_that_only_the_kernel_reads`）；停下的 Task 不被恢复，取消与失效照常结束它（`WorkDecision.held_task_stays_paused_until_it_ends`）；有停下的 Task 的 Goal 不派生，occurrence 以 `TASK_HELD` 等待并按上限跳过，其他 Agent 的 Goal 照常派生，取消该 Task 后恢复派生，Task 表有空位时同样等待，停下 Task 的 step 报告等待结束的时刻，`TASK_HELD` 先于 Task 表已满，多个停下的 Task 都结束后才恢复派生（`WorkDecision.held_task_keeps_its_goal_from_deriving_until_it_ends`、`WorkDecision.held_task_keeps_its_goal_waiting_with_room_in_the_table`、`WorkDecision.held_reason_comes_before_a_full_table`、`WorkDecision.goal_waits_until_its_last_held_task_ends`）；会耗尽版本的结束改为停下，版本已是最大值的 Task 不被决定（`WorkDecision.end_that_would_exhaust_a_revision_holds_the_task`）；顺序、同一 transition 中进入 `READY` 的 Task 按 ID 决定、item 与时间预算（`WorkDecision.tasks_are_decided_in_the_order_they_became_ready`、`WorkDecision.tasks_ready_in_one_transition_are_decided_by_id`、`WorkDecision.step_stops_deciding_once_its_time_budget_is_spent`）；结束的 Task 让出位置后，等待的 occurrence 立即到期（`WorkDecision.decision_that_ends_a_task_makes_room_for_a_waiting_occurrence`）；只有 `RUNNING` 决定（`WorkDecision.only_running_decides`）；回调期间修改 Kernel 的调用被拒绝（`WorkDecision.strategy_cannot_change_the_kernel_while_it_decides`）。
- `tests/unit/strategy_registry_test.cpp`：按 ID 查找、每个 ID 一个版本、容量与逐字节匹配（`StrategyRegistry.*`）；GoalType 的策略解析，缺少时失败且可再次解析（`GoalTypeRegistry.link_strategies_resolves_each_strategy_id`、`GoalTypeRegistry.link_strategies_fails_on_a_missing_strategy_and_may_run_again`）。
- `tests/unit/task_table_test.cpp`、`tests/unit/goal_table_test.cpp`、`tests/unit/trigger_schedule_test.cpp`：完成、失败与停下的 Task 生命周期（`TaskTable.decisions_complete_or_fail_a_ready_task`、`TaskTable.held_task_stays_paused_until_it_ends`）；Task 从停下到结束都算停下，Task 表知道是否有停下的 Task（`TaskTable.task_is_still_held_from_its_hold_until_it_ends`）；按进入 `READY` 的顺序取 Task、`more_ready` 不计版本已是最大值的 Task（`TaskTable.next_ready_takes_the_task_ready_longest_before_the_bound`、`TaskTable.has_ready_counts_tasks_below_the_largest_revision`、`TaskTable.live_task_besides_looks_at_the_other_tasks_of_the_goal`）；Goal 的完成与失败级联到后代（`GoalTable.satisfy_and_fail_end_the_goal_and_cancel_its_live_descendants`）；触发是否还有 occurrence（`TriggerSchedule.occurrence_is_to_come_until_the_trigger_has_had_its_last`）。
- `tests/unit/kernel_work_test.cpp`、`tests/unit/kernel_task_test.cpp`、`tests/unit/kernel_api_test.cpp`、`tests/contract/abi_layout.c`、`tests/contract/c_abi_smoke.c`：状态表中的 `ayni_register_strategy`，回调守卫覆盖它，0.9 布局的 `ayni_goal_type_v1`、`ayni_task_info_v1` 被拒绝，新容量的范围、默认值与内存增长，ABI 0.10 布局，C 调用方注册策略（该 Kernel 没有存储端口，停在 `CONFIGURING`，`decide` 在这里不运行）。
- 私有检查点、步骤与 `step_key`、其余意图种类、`blocked_on`、在途请求的暂停点与取消传播、`STARTING` 与 `FAULTED`、决定的持久化不标为已实现。
