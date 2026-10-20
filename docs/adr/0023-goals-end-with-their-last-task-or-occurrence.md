# ADR-0023：Goal 随最后一个 Task 的取消或最后一个 occurrence 的跳过结束

- **状态**：已接受（M1 已实现）
- **日期**：2026-10-20
- **关联**：计划 §2.2、§2.4、§5.3；`MA-004`；ADR-0018、ADR-0020、ADR-0022；C-LIFE-022、C-RES-005、C-COMMIT-012、C-VER-006

## 背景

ADR-0022 让 Goal 随它最后一个被决定完成或失败的 Task 结束，但另外两条路仍让 Goal 停在 `ACTIVE`：`TASK_CANCEL` 只取消一个 Task，Goal 不变（ADR-0018 决策 5）；等待超限只跳过 occurrence，不改生命周期（ADR-0020 决策 5、6）。一次性 Goal（`ONCE`、`DELAY`）只有一个 occurrence，它的 Task 被取消或 occurrence 被跳过之后不会再有 Task，却仍是 `ACTIVE`：查询看不出它已无事可做，调用方必须另发 `GOAL_CANCEL`，它的子目标也一直保持活动。

ADR-0022 后果中记下了这一点，是否让 Goal 随之结束已另请用户决定。本 ADR 按推荐项“随之结束”实现，用户改选“保持活动”时修订本 ADR 与相应实现。（2026-10-20 用户确认了推荐项“随之结束”。）

## 决策

1. **最后一个 Task 被取消时 Goal 随之取消（产品语义，推荐项）。** `TASK_CANCEL` 取消一个 `READY` 或 `PAUSED` 的 Task 后，若它的 Goal 没有其他未结束的 Task，也不会再有 occurrence，同一 transition 中 Goal 进入 `CANCELLED` 并记录 `cancel_revision`，像 `GOAL_CANCEL` 一样取消未终结的后代与它们的 Task。判定与 ADR-0022 中决定结束 Task 时相同，结局随 Task 的结束方式：完成为 `SATISFIED`，失败为 `FAILED`，取消为 `CANCELLED`。`PAUSED` 的 Task 的 Goal 可能是 `ACTIVE`（祖先或 owner 暂停，或策略的决定使 Task 停下），也可能自己是 `PAUSED`；取消对二者都适用。实际上主要是 `ONCE`、`DELAY` Goal 这样结束：周期 Goal 要到最后一个编号（2^32−1），或下一次到期时刻超出时钟范围（周期极长时），本地输入 Goal 要到最后一个编号，才没有下一次 occurrence；否则取消其中一个 Task 后照常继续。意图的结果仍指向 Task，Goal 与后代的变化在查询中可见。
2. **最后一个 occurrence 被跳过时 Goal 失败（产品语义，推荐项）。** `ayni_step` 跳过等待超限的 occurrence（ADR-0020）后，若 Goal 不会再有 occurrence，也没有未结束的 Task，同一 transition 中 Goal 进入 `FAILED`，取消未终结的后代与它们的 Task。`occurrences_skipped` 照常计数，`skip_reason` 保留跳过的原因（`TASK_CAPACITY` 或 `ID_EXHAUSTED`）。结局是 `FAILED` 而不是 `CANCELLED`：没有人取消它，是 Kernel 没能为它派生 Task，原因可查，与决定失败的 Task 使 Goal 失败一致。跳过时仍有未结束的 Task（例如以 `TASK_HELD` 跳过时停下的 Task），Goal 保持 `ACTIVE`，由那个 Task 的结束决定 Goal 的结局（决策 1、ADR-0022）。
3. **版本。** 结束 Goal 的 `TASK_CANCEL` 的上限检查覆盖 Goal 的结束会提升的全部版本：Goal、未终结的后代与它们的 Task；任一已是 2^32−1 时整个意图以 `CAPACITY_EXCEEDED`、`REVISION_EXHAUSTED` 拒绝，`object` 仍是 Task，不改变任何对象（C-COMMIT-012）。跳过是 Kernel 自身的推进，不能拒绝：结束会使版本越过最大值时只跳过，Goal 保持 `ACTIVE` 且不会再有 occurrence，与放弃失效的处理相同（ADR-0020）。跳过本身仍不提升版本；Goal 因跳过而失败时，失败提升 Goal 与后代的版本，调用方持有的版本随之过期，但这时 Goal 已是终态，暂停或取消本来也会因状态被拒绝，ADR-0020 决策 6 的理由不受影响。
4. **ABI 0.11。** 布局不变，只改语义：`TASK_CANCEL` 可能结束 Goal，跳过可能使 Goal 失败。按 C-VER-006，0.x 中改变已发布函数的语义也要递增 minor，0.10 的调用方须重新编译。`ayni.h` 中 `TASK_CANCEL`、`ayni_step`、触发与 `skip_reason` 的说明标明自 0.11。

决策 3、4 为实施细则，未单独请用户决定。

## 后果

- 一次性 Goal 不再停在“`ACTIVE` 却不会再有 Task”：取消它的 Task 即结束它，等待超限即失败；查询看到终态，子目标随之结束，不必再发 `GOAL_CANCEL`。
- 不能再只取消一次性 Goal 的 Task 而保留 Goal；要重做，调用方创建新 Goal。依赖“`TASK_CANCEL` 不改变 Goal”或“跳过不改变 Goal 生命周期”的调用方须调整，Goal 的 `revision` 与 `commit_seq` 会随之改变。
- 周期与本地输入 Goal 不受影响：取消一个 Task 或跳过 occurrence 后照常继续，除非如决策 1 所说它们也不会再有 occurrence。
- 判定先看 Goal 是否还会有 occurrence，不会再有时才遍历一次 Task 表；结束 Goal 时的上限检查与 `GOAL_CANCEL` 相同。Kernel 的内存需求不变（默认配置 40,024 字节）。
- 一个 Goal 因 Task 失败还是因跳过而失败，`skip_reason` 不能完全区分：周期 Goal 先跳过过 occurrence、后来最后一个 Task 失败时，`skip_reason` 也有值。需要时再按备选方案中的结束原因另议。
- Goal 失效（`EXPIRED`）与实例停止不变。

## 备选方案

- **保持活动**：Goal 留在 `ACTIVE`，由调用方另发 `GOAL_CANCEL`。行为与 ABI 都不变，但 Goal 的状态与它能做的事不符，子目标也一直保持活动。待用户决定，当前未选。（2026-10-20 用户确认“随之结束”，此项未选。）
- **跳过时进入 `CANCELLED`**：没有人取消它，`cancel_revision` 记录的是取消它的版本，含义不符。未选。
- **跳过时进入 `EXPIRED`**：失效在本库中指 Goal 自身的失效时刻（ADR-0020），混用后调用方无法区分。未选。
- **新增 Goal 的结束原因**：可以区分跳过失败与 Task 失败，但需要新的原因码与布局变更，`skip_reason` 已给出跳过的原因。未选，需要时再议。
- **结束会耗尽版本时拒绝跳过**：跳过是 Kernel 的推进，拒绝后 occurrence 一直到期而无法处理，每次 step 都会重试。否决。
- **结束会耗尽版本时只取消 Task、保留 Goal**：意图的效果会取决于调用方看不到的上限，不符合 C-COMMIT-012 的“整个意图不改变任何对象”。否决。

## 验证

- `tests/unit/kernel_task_test.cpp`（`WorkTask.*`）：取消一次性 Goal 唯一的 Task 时，Goal、后代与后代的 Task 在同一 transition 中取消，其他 Goal 不变（`WorkTask.task_cancel_of_a_one_shot_goals_task_cancels_the_goal`）；Goal 自己暂停、祖先暂停、owner 暂停，或 Task 被策略停下而 Goal 暂停时同样取消（`WorkTask.task_cancel_cancels_a_one_shot_goal_whatever_holds_its_task`）；周期 Goal 取消一个 Task 后继续且版本不变，不会再有 occurrence 时只随最后一个未结束的 Task 结束（`WorkTask.task_cancel_ends_a_goal_only_with_its_last_task`）；下一次到期时刻超出时钟范围的周期 Goal 随它的 Task 取消（`WorkTask.task_cancel_ends_an_interval_goal_whose_next_time_never_comes`）；Goal 的结束会耗尽 Goal、后代或后代的 Task 的版本时整个意图被拒绝，不结束 Goal 的取消照常进行（`WorkTask.task_cancel_that_would_exhaust_a_revision_its_goals_end_raises_is_refused`）；`TASK_CANCEL` 仍唤醒 owner（`WorkTask.intents_that_may_let_a_task_derive_wake_the_owner`）。
- `tests/unit/kernel_event_test.cpp`：本地输入 Goal 的 Task 被取消后 Goal 保持 `ACTIVE`，下一次输入照常派生（`WorkInput.task_cancel_leaves_the_goal_taking_inputs`）。
- `tests/unit/kernel_expiry_test.cpp`（`WorkWait.*`）：跳过最后一个 occurrence 使 Goal 失败，保留计数与原因，后代与其 Task 在同一 transition 中取消，让出的位置在同一 step 中给等待的 Goal，调用方持有的版本过期（`WorkWait.skipping_the_last_occurrence_fails_the_goal`、`WorkWait.used_up_ids_skip_with_their_reason`）；跳过时仍有未结束的 Task 的 Goal 保持 `ACTIVE`，该 Task 被取消时 Goal 随之取消（`WorkWait.skip_leaves_a_goal_with_a_live_task_active`）；结束会耗尽 Goal、后代或后代的 Task 的版本时只跳过（`WorkWait.skip_whose_end_would_exhaust_a_revision_leaves_the_goal_active`）。
- `tests/unit/kernel_strategy_test.cpp`：取消一次性 Goal 停下的 Task 时 Goal 随之取消（`WorkDecision.held_task_stays_paused_until_it_ends`）。
- `tests/unit/task_table_test.cpp`：不排除任何 Task 时查找 Goal 未结束的 Task（`TaskTable.live_task_besides_looks_at_the_other_tasks_of_the_goal`）。
