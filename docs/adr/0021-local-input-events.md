# ADR-0021：工作模型第二部分（四）：事件投递与本地输入触发

- **状态**：已接受（`MA-004` 第二部分的第四个 PR 在 M1 已实现）
- **日期**：2026-10-20
- **关联**：计划 §2.1、§2.2、§3.2「线程」「生命周期」、§4.2、§5.3；`MA-003`、`MA-004`、`MA-010`、`MA-011`、`MA-018`、`MA-019`；ADR-0015、ADR-0016、ADR-0018、ADR-0019、ADR-0020；C-HOST-008、C-HOST-018、C-HOST-020、C-HOST-021、C-SUB-004、C-LIMIT-007、C-LIMIT-008、C-POLICY-009、C-VER-002、C-VER-003、C-PROD-018

## 背景

ADR-0018 把 `MA-004` 第二部分分为 Task 与传播、时间、事件三个 PR，本 ADR 是事件 PR。`MA-003` 已实现有界事件队列与入口临界区（ADR-0015），但 `ayni_post_event` 仍在草案中，队列只有 Kernel 内部使用；Goal 的触发只有时间（ADR-0019、ADR-0020）。计划 §2.1 要求联网模型不可用时仍能处理本地按键与规则事件，E1 步骤 2 要向 Agent 注入本地按键事件。

本 PR 不拆成“只有入口”与“触发”两个 PR：只有入口时事件的来源字段没有使用者，事件被 `step` 取出后也只能计数丢弃，测试无法说明入口的语义。因此 `ayni_post_event` 与本地输入触发在同一个 PR 中实现。

本 ADR 只实现本地输入一种事件。观察、遥测与时钟可信事件依赖订阅代际、资源与时钟可信度（`MA-018`、`MA-011`），留在草案；事件正文、订阅字段与 Host 采样的入队时刻随它们追加。Goal、Task 与已入队的事件仍只在内存中，重启后的处理随 `MA-010`。

其中两个产品语义问题已请用户决定（决策 5、6）；本 ADR 按推荐项实现，用户改选时修订本 ADR 与相应实现。

## 决策

1. **公开事件结构（ABI 0.9）。** `ayni_event_v1` 含 `struct_size`、`kind` 与 `source`，`ayni_post_event` 复制事件后返回。M1 只实现 `AYNI_EVENT_LOCAL_INPUT`（3）：按键、本地规则等本机输入，`source` 是命名该输入的目录 ID（如 `button.record`），规则与 GoalType ID 相同。草案中的观察（1）、遥测（2）与时钟可信（4）返回 `AYNI_UNSUPPORTED`。这是新结构，只接受自身的 `sizeof`。
2. **检查顺序与线程。** 依次为：空指针、`struct_size` 过小或 `kind` 为 0 返回 `AYNI_INVALID_ARGUMENT`，`struct_size` 过大返回 `AYNI_UNSUPPORTED_VERSION`；未实现的种类返回 `AYNI_UNSUPPORTED`；来源不是目录 ID 返回 `AYNI_INVALID_ARGUMENT`；Kernel 不接收事件时（决策 3）返回 `AYNI_INVALID_STATE`；队列已满返回 `AYNI_BUSY`。可从任意非 ISR 线程调用（C-HOST-008），不得从 Kernel 调用的函数内调用，也不与 `ayni_create`、`ayni_destroy` 并发（C-HOST-018）。只有入队成功才在解锁后调用一次 `wake`（C-HOST-020）。
3. **事件门。** `ayni_post_event` 运行在 Host 线程上，不读所有者线程的生命周期字段。事件入口另有一个“接收事件”标志，与队列一同在入口锁内读写：`ayni_start` 打开它，进入 `STOPPED` 时关闭它，关闭后不再打开。所以 `CONFIGURING` 与 `STOPPED` 返回 `AYNI_INVALID_STATE`，其余状态返回 `AYNI_OK`。关闭时仍在队列中的事件不会再被取出，计为丢弃。
4. **各状态如何处理事件。** `step` 先应用完成、推进恢复，再取出它开始取事件时已在队列中的事件，每个算一个 item，之后才做到期工作；`step` 运行期间投递的事件留给下一次。`RUNNING` 应用事件（决策 5）；`DRAINING` 与 `RESTRICTED` 不接收新工作（ADR-0018），丢弃事件并计数；`RECOVERING` 尚无 Goal，事件留在队列中，进入 `RUNNING` 后应用，这期间不报告 `more_ready`，恢复的推进本身会唤醒 owner。同一 step 中进入 `RUNNING` 时，事件在该 step 中就被应用。
5. **按来源匹配（产品语义，推荐项）。** 新增触发种类 `AYNI_TRIGGER_LOCAL_INPUT`（4），GoalType 须在 `trigger_kinds` 中声明接受它（C-POLICY-009）。`ayni_goal_create_v1` 追加 `trigger_source`：本地输入触发须给出目录 ID，其他种类须为空；本地输入触发不带延时与周期。事件到达时，Kernel 在一个 transition 中为每个来源相同、尚未终结（`PROPOSED`、`ACTIVE`、`PAUSED`）且还有编号的 Goal 记录一次 occurrence，到期时间为取出它的 step 的开始时刻。暂停的 Goal 同样记录并保留，与 ADR-0019 决策 3“计时不因暂停而停止”一致；终结的 Goal 不记录，Goal 进入终态时丢弃尚未取走的 occurrence。没有 Goal 接收的事件不产生 transition，计入 `ayni_kernel_info_v1.events_unmatched`。
6. **等待的输入合并为一个 Task（产品语义，推荐项）。** Goal 尚未取走的 occurrence 与时间触发一样合并：下一次派生的 Task 代表全部等待的 occurrence，`occurrence` 为最新的编号，`occurrence_count` 为个数，`due_us` 为最新一次输入的到期时间（C-LIMIT-007、ADR-0019）。所以 Goal 暂停期间或 Task 表满时连按三次，得到一个代表三次的 Task。
7. **记录不提升版本。** 与派生、跳过一样（ADR-0018、ADR-0020），记录只改触发游标与 `commit_seq`，调用方持有的版本仍可用。触发游标另记等待的个数与最新一次的到期时间；`ayni_goal_info_v1.next_due_us` 为最早一次等待的到期时间，没有等待时为 `UINT64_MAX`。创建本地输入触发的 Goal 时没有到期的 occurrence，所以不唤醒 owner。
8. **编号用尽结束触发。** occurrence 仍从 1 编号；编号到 2^32−1 后 Goal 不再记录输入，与时间触发的编号用尽相同（ADR-0019），已记录的仍可取走。若没有其他 Goal 接收，该输入计为未匹配。
9. **有界等待照常适用。** 等待从最早一次等待的 occurrence 到期起算（ADR-0020 决策 7）；超限时跳过全部已记录的 occurrence，计数并记原因。`wait_reason` 在有 occurrence 等待时说明派生会被什么挡住，没有等待时为 `NONE`。失效与其他触发种类相同。
10. **队列条目保存来源。** 本地输入不可合并、不可丢弃（C-SUB-004），队列满时返回 `AYNI_BUSY` 由 Host 稍后再投，不挤掉其他事件。来源存放在队列条目的正文字节中，所以每个条目的正文空间至少为 `AYNI_CATALOG_ID_MAX_BYTES`，`event_body_max_bytes` 更小时按 32 字节计算内存需求。
11. **诊断计数（ABI 0.9）。** `ayni_kernel_info_v1` 追加 `events_unmatched` 与 `events_discarded`；后者包括 `DRAINING`、`RESTRICTED` 中丢弃的事件和进入 `STOPPED` 时仍在队列中的事件。`ayni_goal_info_v1` 保留 `reserved`，在其后追加 `trigger_source_size` 与 `trigger_source`（C-VER-002）。

决策 5 中“一个输入到达全部匹配的 Goal”、决策 4 中三种状态的处理、决策 10 的存放方式与决策 11 的计数为实施细则，未单独请用户决定。

## 后果

- 本地按键与规则可以在订阅（`MA-018`）之前触发 Goal：Host 投递本地输入，`step` 记录 occurrence 并派生 Task。E1 步骤 2 的本地按键注入有了入口。
- 来源不登记。拼错来源的 Goal 永远不会被触发，只能从 `events_unmatched` 看出事件没有接收者；资源与订阅建立后，是否要求来源已登记随 `MA-018` 决定。
- occurrence 的到期时间是 `step` 开始的时刻，不是投递的时刻。事件尚不携带 Host 采样的入队时刻，`contracts/domain/limits.md` 的本地事件处理延迟在追加该字段后测量。
- `RECOVERING` 期间投递的事件占用队列，恢复较长时 Host 会收到 `AYNI_BUSY`，这是预期的背压；停机期间的按键被丢弃并计数。
- `ayni_post_event` 无法判断自己是否在 Kernel 调用的函数内运行（该标志属于所有者线程的状态），这条限制只写在文档中。
- 每个队列条目至少保留 32 字节正文：默认配置（正文 128 字节）不增加内存，正文配置小于 32 字节的队列按 32 字节计算。
- 0.8 的调用方须重新编译：主版本 0 下不同 minor 互不接受（C-VER-006），`ayni_goal_create_v1`、`ayni_goal_info_v1` 只接受 0.9 的大小；`ayni_kernel_info_v1` 仍接受旧布局，只写其覆盖的完整字段。

## 备选方案

- **先做只有入口的 PR**：来源字段没有使用者，取出的事件只能丢弃，测试说明不了语义。否决，见背景。
- **等订阅（`MA-018`）再做事件触发**：本地按键提醒要等到资源与订阅登记之后才能工作，E1 的本地事件也无从注入。未选；订阅建立后可以再约束 Goal 能使用的来源。
- **事件直接指定 Goal**：Host 要知道 Goal ID，一次按键无法同时服务多个 Goal，Goal 重建后 Host 也要跟着改。未选。
- **只触发第一个匹配的 Goal**：哪个 Goal 先匹配取决于表中位置，结果对用户不可解释。否决。
- **每次输入各派生一个 Task**：按键抖动或长按会占满 Task 表，与 C-LIMIT-007 的合并规则不一致。未选。
- **Goal 无法取走时丢弃输入并计数**：暂停期间的按键会丢失，与错过的 occurrence 补发一次、不丢弃（C-PROD-018）的方向相反。未选。
- **`RESTRICTED`、`DRAINING` 保留事件**：`RESTRICTED` 不重启就不会回到 `RUNNING`，停机期间也不接收新工作，保留只会占满队列。否决。
- **`RECOVERING` 丢弃事件**：启动期间的按键会丢失，而 Kernel 很快就进入 `RUNNING`。否决。
- **队列条目另设来源字段**：每个条目多约 36 字节，默认 128 个条目约多 4.6 KB，而正文空间已经存在。否决。
- **`ayni_post_event` 读取生命周期字段**：该字段只属于所有者线程，跨线程读取是数据竞争。否决，用入口锁保护的事件门。

## 验证

- `tests/unit/kernel_event_test.cpp`（`KernelEvents.*`）：六个生命周期状态中投递的返回值与唤醒（`KernelEvents.states_between_start_and_stopped_take_posts`）；参数、种类、来源先于状态检查，失败不唤醒、不入队（`KernelEvents.post_checks_arguments_kind_and_source_before_the_state`）；队列满返回 `AYNI_BUSY`，step 取走后可再投（`KernelEvents.full_queue_refuses_until_a_step_takes_one`）；item 与时间预算（`KernelEvents.events_beyond_the_step_budget_wait_for_the_next_step`）；`RECOVERING` 保留事件且不报告 `more_ready`（`KernelEvents.recovering_keeps_events_for_running`）；`RESTRICTED`、`DRAINING` 丢弃并计数（`KernelEvents.restricted_and_draining_discard_events`）；进入 `STOPPED` 时仍在队列中的事件计为丢弃，之后投递被拒绝（`KernelEvents.stopping_counts_the_events_left_queued`）；多个 Host 线程并发投递，每个成功的投递恰好取出一次、唤醒一次，持锁期间没有 Host 调用（`KernelEvents.posts_from_host_threads_are_each_taken_once`）；投递与停机竞争时每个成功的投递恰好计数一次（`KernelEvents.posts_racing_shutdown_are_each_counted_once`）。
- 同一文件（`WorkInput.*`）：Goal 只接收自己来源的输入，创建时不唤醒，记录不改版本（`WorkInput.goal_takes_inputs_from_its_source`）；一个输入在一个 transition 中到达全部匹配的 Goal（`WorkInput.one_input_reaches_every_matching_goal_in_one_transition`）；暂停期间的输入在恢复后合并为一个 Task（`WorkInput.paused_goal_backlog_becomes_one_task`）；终结的 Goal 不接收（`WorkInput.terminal_goals_take_nothing`）；有界等待超限时全部跳过（`WorkInput.inputs_waiting_too_long_are_skipped`）；正文配置为 1 字节时仍能保存 32 字节的来源，内存需求与 32 字节时相同（`WorkInput.small_event_bodies_still_hold_a_source`）；GoalType 须声明接受本地输入（`WorkInput.goal_type_must_accept_local_input`）。
- `tests/unit/trigger_schedule_test.cpp`、`tests/unit/goal_table_test.cpp`：记录与合并取走、编号用尽结束记录（`TriggerSchedule.recorded_inputs_are_taken_together`、`TriggerSchedule.last_occurrence_number_ends_recording`）；匹配只针对来源相同且未终结的 Goal、记录改 `commit_seq` 不改版本、终结时丢弃等待的输入（`GoalTable.input_is_recorded_on_every_live_goal_with_its_source`、`GoalTable.terminal_and_exhausted_goals_take_no_input`）。
- `tests/unit/kernel_work_test.cpp`、`tests/unit/kernel_lifecycle_test.cpp`、`tests/contract/abi_layout.c`、`tests/contract/c_abi_smoke.c`：来源字段的校验，0.8 布局的 `ayni_goal_create_v1`、`ayni_goal_info_v1` 被拒绝，0.5 起的 `ayni_kernel_info_v1` 旧布局只写覆盖的字段，ABI 0.9 布局。
- 观察、遥测与时钟可信事件，事件正文与入队时刻，事件与 occurrence 的持久化不标为已实现。
