# ADR-0024：工作模型第三部分（二）：步骤与私有检查点

- **状态**：已接受（`MA-004` 第三部分的第二个 PR 在 M1 已实现）
- **日期**：2026-10-20
- **关联**：计划 §2.4、§2.5、§3.2「新策略」、§4.2、§5.3；`MA-004`、`MA-008`、`MA-009`、`MA-010`；ADR-0006、ADR-0014、ADR-0018、ADR-0022、ADR-0023；C-POLICY-002、C-POLICY-003、C-POLICY-004、C-POLICY-006、C-POLICY-007、C-POLICY-008、C-CALL-004、C-ID-003、C-LIFE-021、C-LIFE-023、C-LIFE-024、C-LIMIT-006、C-COMMIT-012、C-VER-006

## 背景

ADR-0022 决策 1 把 `MA-004` 第三部分分为两个 PR，本 ADR 是第二个：步骤与检查点。ADR-0022 中一个决定只能结束 Task：策略无法记下做到了哪一步，也无法在两次决定之间保存自己的状态，所以一个 Task 必须在一次决定中结束。

C-CALL-004 的共同检查点保存步骤版本、绑定、输入摘要、结果引用、已提交进度与未知状态。其中绑定、输入摘要、结果引用、未知状态与 `logical_call_id`（C-ID-003）都属于调用，要等 `MA-008`；检查点的持久化与重启后的兼容要等 `MA-010`。没有调用时，共同检查点中能实现并验证的只有已提交的进度，即哪些步骤已经完成。

两个问题已请用户决定，用户都选了推荐项：只改私有检查点、不报告新步骤的决定使 Task 停下（决策 6）；Task 信息先只给步骤数（决策 9）。

## 决策

1. **范围。** 本 PR 实现私有检查点、`REPORT_PROGRESS` 意图与 Task 已提交的步骤（共同检查点中的已提交进度），以及不结束 Task 的决定。`logical_call_id`、绑定、输入摘要、结果引用与未知状态随调用（`MA-008`）追加在 `ayni_task_step_v1` 末尾。检查点的持久化与重启后的兼容检查（C-POLICY-007 的“版本不兼容或策略未知”、C-LIFE-024）随 `MA-010`：同一次启动中只有一个版本的策略决定一个 Task（C-POLICY-010），不会遇到不兼容的检查点。
2. **步骤（ABI 0.12）。** `REPORT_PROGRESS`（4）报告一个步骤已经完成，以内联的 `step_key` 指名：`step_key_size` 与 32 字节的 `step_key`，内容是目录 ID，不以 0 结尾。用内联数组而不用指针，是因为 Kernel 在 `decide` 返回之后才提交步骤，指针会指向策略的内存，策略要让它在调用之外仍然有效。步骤在 Task 内唯一（C-POLICY-008），只增不删，按提交顺序保存；每个 Task 最多 `task_step_capacity` 个（默认 8，1–64）。快照追加 `step_count`、`step_capacity` 与 `steps`（`ayni_task_step_v1` 数组，视图指向 Kernel 内存，只在本次调用中有效）。结束意图的 `step_key_size` 必须为 0。一个决定可以含多个 `REPORT_PROGRESS` 与至多一个结束意图，顺序不限；结束 Task 的决定也提交它的步骤，与结束在同一 transition 中。
3. **不结束 Task 的决定。** 至少含一个 `REPORT_PROGRESS`、没有结束意图的决定被接受时，Task 保持 `READY`，版本加 1，`commit_seq` 取该 transition 的值，Goal 不变。ADR-0022 决策 5 的顺序不变：一次 step 只决定 step 开始前已是 `READY` 的 Task，所以这个 Task 在之后的 step 中再被决定，排在在它之前已是 `READY` 的 Task 之后，一个 Task 每次 step 至多被决定一次。
4. **私有检查点。** `ayni_strategy_v1` 追加 `max_checkpoint_bytes`（0 到 `task_checkpoint_max_bytes`）与必须为 0 的 `reserved`；`task_checkpoint_max_bytes` 默认 64，0–4096。快照的 `checkpoint`（`ayni_private_checkpoint_v1`）带写入它的策略的 ID、版本与内容；第一个被应用的决定之前为空，版本为 0。输出追加 `checkpoint_capacity`、`checkpoint_size` 与 `checkpoint`：Kernel 提供的缓冲先复制 Task 的私有检查点，到策略容量为止的其余字节清零，`checkpoint_size` 为它的大小；策略可以不动，也可以写入新的检查点。每个被应用的决定（结束 Task 或使它继续）都以缓冲中的前 `checkpoint_size` 字节替换 Task 的私有检查点，并记下策略；被拒绝的决定保留原来的检查点。Kernel 只读回 `checkpoint_size` 与自己的缓冲，并按自己保存的 `max_checkpoint_bytes` 检查大小，所以策略改写 `checkpoint` 指针或 `checkpoint_capacity` 也不会让 Kernel 读别处或读得更多（C-POLICY-003）；清零使一个 Task 看不到另一个 Task 的决定留在缓冲中的内容。私有检查点只保存策略自己的状态；步骤属于 Kernel，策略只能追加，不能改写或删除（C-CALL-004）。
5. **检查顺序。** 在 ADR-0022 决策 9 的顺序上：`intent_count` 超过上限或 `checkpoint_size` 超过策略的 `max_checkpoint_bytes` 都是 `CAPACITY_EXCEEDED`、`POLICY_OUTPUT_LIMIT`，在“没有意图”之前检查；之后逐个意图依次检查结构大小、种类与版本。`REPORT_PROGRESS` 再依次检查 `step_key` 是目录 ID（否则 `INVALID_ARGUMENT`、`POLICY_INTENT`），Task 与前面的意图都没有提交这个步骤（否则 `INVALID_ARGUMENT`、`STEP_TAKEN`，32），以及 Task 还有空位（否则 `CAPACITY_EXCEEDED`、`STEP_CAPACITY`，33）；结束意图检查 `step_key_size` 为 0 且前面没有结束意图（否则 `INVALID_ARGUMENT`、`POLICY_INTENT`）。最后是会使版本越过最大值的结束。32、33 只用于 Task 信息。被拒绝的决定整体不应用，不提交任何步骤与私有检查点（C-LIMIT-006）。一个不合法的 `step_key` 不可能与已提交的步骤相同，所以 `POLICY_INTENT` 与 `STEP_TAKEN` 的先后从外部观察不到；先检查 `step_key`，使查重只比较合法的 ID。
6. **只改私有检查点的决定使 Task 停下（产品语义，用户选定“停下”）。** 没有意图的决定仍以 `INVALID_ARGUMENT`、`POLICY_NO_PROGRESS` 停下 Task，即使它改了私有检查点，该检查点也不提交。因此每个不停下 Task 的决定要么结束它，要么提交至少一个新步骤；步骤唯一且有上限，一个 Task 至多被应用 `task_step_capacity` + 1 个决定，只报告进度的策略在第 `task_step_capacity` + 1 个决定时以 `STEP_CAPACITY` 停下。另一个选项“继续”允许策略只更新算法状态再被决定，但从不报告进度的策略会让 Task 一直被决定，`more_ready` 不再清零，与 ADR-0022 决策 10 否决“拒绝后再问”的理由相同。
7. **存储。** 步骤唯一且不超过上限由 Task 表保证：`add_step` 拒绝重复、不合法或超出上限的步骤；决定在应用之前已逐个检查，所以这些拒绝只是兜底。每个 Task 有一个槽位，存放它的 `task_step_capacity` 个步骤（每个 36 字节：32 字节的 ID 与长度）与 `task_checkpoint_max_bytes` 字节的私有检查点，与 Task 记录分开。结束的 Task 让位时，Task 表把末尾的 Task 移入空出的下标（ADR-0018），槽位随 Task 记录移动，步骤与检查点不复制；新 Task 使用它所替换的 Task 的槽位，从零个步骤与空检查点开始。表未满时，槽位 0 到 size−1 都在使用。决定用的缓冲（步骤视图、意图与检查点）只有一份，所有策略共用，因为决定逐个进行。
8. **暂停保留步骤与检查点。** Goal、祖先或 owner 实例暂停时 Task 停下（ADR-0018），步骤与私有检查点保留，恢复后的下一次决定从它们开始（C-LIFE-021、C-LIFE-023 的“保留检查点”）。策略的决定使 Task 停下时同样保留；结束的 Task 保留到它让出位置。
9. **Task 信息只给步骤数（实施细则，用户选定“先给步骤数”）。** `ayni_task_info_v1` 追加 `step_count` 与 `reserved`，owner 可以看到 Task 已提交几个步骤。逐条查询步骤的接口随 `MA-008`，那时步骤还带调用身份与结果，一并设计。
10. **ABI 0.12。** `ayni_config_v1`、`ayni_task_info_v1`、`ayni_task_snapshot_v1`、`ayni_policy_intent_v1`、`ayni_policy_output_v1` 与 `ayni_strategy_v1` 在末尾追加字段。0.x 的小版本必须一致（C-VER-006），0.11 的调用方与策略须重新编译；`ayni_config_v1` 仍接受旧布局，未覆盖的新容量取默认值。语义变化是一个决定可以使 Task 保持 `READY`，它在之后的 step 中再被决定。`REPORT_PROGRESS`、`step_key` 与私有检查点从草案 `include/ayni/draft/policy_v1.h` 移入 `ayni.h`。
11. **版本不回绕。** 使 Task 继续的决定也提升它的版本。版本已是最大值的 Task 不被决定（ADR-0022 决策 11），所以继续总能应用；在 2^32−2 上继续的 Task 停在最大版本的 `READY`，不再被决定，也不计入 `more_ready`。
12. **内存。** 默认配置下 Kernel 的内存需求从 40,024 字节增至 46,432 字节（+6,408）：16 个 Task 各增 368 字节（8 个步骤 288 字节、64 字节私有检查点、Task 记录增 16 字节），决定的缓冲增 480 字节（8 个步骤视图 128 字节、8 个意图各增 36 字节、64 字节检查点），Kernel 状态中 Task 表与缓冲的对象增 40 字节。工作模型取最小容量（每个 Task 1 个步骤、没有私有检查点）时从 26,532 字节增至 26,680 字节。增量随 `task_capacity ×（task_step_capacity × 36 + task_checkpoint_max_bytes）` 增长，三者都取最大值时 Task 表的块约 1.6 MB，配置方按设备内存选择。

决策 2–5、7、8、10–12 为实施细则，未单独请用户决定。

## 后果

- 策略可以分多次决定推进一个 Task：每次报告完成的步骤，并在私有检查点中保存自己的状态，同一 step 中的其他 Task 不受影响。E1 的规则策略与 E3 的检查点、策略替换实验有了入口。没有调用时，M1 的步骤只是策略声明完成的名字，不代表 Kernel 观察到的效果；调用的步骤随 `MA-008`。
- 一个 Task 被应用的决定有上限 `task_step_capacity` + 1，策略的缺陷不会无限占用 step。代价是需要更多步骤的 Task 要配置更大的 `task_step_capacity`，或分成子目标。
- 步骤与检查点仍只在内存中，重启后丢失；C-POLICY-007 的版本兼容与 C-LIFE-024 未实现，随 `MA-010`。
- 一次决定现在还要复制检查点与查重步骤：每个 `REPORT_PROGRESS` 与 Task 已提交的步骤和前面的意图各比较一次，每次至多 32 字节，取最大容量时一次决定至多约 8,000 次比较。时间预算仍在每个决定之后检查（ADR-0022 决策 5）。
- 0.11 的调用方与策略须重新编译。

## 备选方案

- **本 PR 只做私有检查点，步骤等调用（`MA-008`）**：没有进度意图时一个决定只能结束 Task，私有检查点只在一次调用内有用，而一次调用内不需要检查点。未选，见决策 1。
- **`step_key` 为指向策略内存的视图**：Kernel 要在 `decide` 返回后读取策略的内存。否决，见决策 2。
- **策略提供私有检查点的缓冲**：与 ADR-0022 否决策略提供意图缓冲的理由相同：Kernel 要校验策略给出的指针与长度。否决。
- **只改私有检查点的决定使 Task 继续**：用户否决，见决策 6。
- **不限步骤数，改为限制决定次数**：要另记计数，次数与进度无关；步骤唯一已经给出上限。未选。
- **步骤与检查点放在 Task 记录中**：Task 表移动 Task 时要复制整块，按 Task 查找时遍历的记录也随容量变大。未选，见决策 7。
- **被拒绝的决定也提交私有检查点**：拒绝除停下以外不应改变 Task，原检查点也便于诊断。否决。
- **结束 Task 的决定不提交步骤**：结束前的最后一步会丢失，Task 信息的步骤数少于策略报告的。否决。
- **立即提供逐条查询步骤的接口**：用户选定先给步骤数，见决策 9。

## 验证

- `tests/unit/kernel_checkpoint_test.cpp`（`WorkCheckpoint.*`）：报告进度的决定提交步骤、Task 保持 `READY` 并在下一次 step 再被决定，快照按提交顺序给出步骤（`WorkCheckpoint.progress_commits_steps_and_leaves_the_task_ready`）；结束 Task 的决定在同一 transition 中提交步骤并替换私有检查点（`WorkCheckpoint.decision_that_ends_its_task_commits_its_steps`）；以整个 `step_key` 区分步骤，前缀相同的是不同步骤（`WorkCheckpoint.steps_that_share_a_prefix_are_distinct`）；私有检查点带策略 ID 与版本回到下一次决定，输出从它的副本开始，其余字节清零，空检查点仍属于该策略（`WorkCheckpoint.private_checkpoint_comes_back_with_the_next_decision`）；Kernel 只读自己的缓冲与自己保存的上限，一个 Task 看不到另一个 Task 留下的内容（`WorkCheckpoint.kernel_reads_only_its_own_checkpoint_buffer`）；十五种被拒绝的决定各以其状态码与原因停下 Task，不提交步骤与检查点（`WorkCheckpoint.refused_decisions_commit_nothing_and_hold_with_their_reason`），会使版本越过最大值的结束也是如此（`WorkCheckpoint.end_that_would_exhaust_a_revision_commits_nothing`）；只改私有检查点的决定使 Task 停下（`WorkCheckpoint.decision_that_only_changes_the_checkpoint_holds_the_task`）；步骤限制决定次数（`WorkCheckpoint.steps_bound_how_often_a_task_is_decided`）；继续的 Task 排在在它之前已是 `READY` 的 Task 之后（`WorkCheckpoint.progress_puts_the_task_behind_the_tasks_ready_before_it`）；Goal 与实例的暂停保留步骤与检查点（`WorkCheckpoint.pause_keeps_the_steps_and_checkpoint_for_the_next_decision`）；占用让出位置的新 Task 从空开始（`WorkCheckpoint.task_in_a_given_up_place_starts_without_steps_or_checkpoint`）；继续到最大版本的 Task 不再被决定（`WorkCheckpoint.progress_to_the_largest_revision_leaves_the_task_undecided`）。
- `tests/unit/kernel_strategy_test.cpp`：注册检查 `max_checkpoint_bytes` 与 `reserved`（`WorkStrategy.registration_checks_its_arguments`）；快照与输出的新字段、Task 信息的 `step_count`（`WorkDecision.task_is_decided_in_the_step_after_it_became_ready`）；每次决定从清零的意图开始，`step_key` 也清零（`WorkDecision.strategy_starts_from_cleared_intents_that_only_the_kernel_reads`）。
- `tests/unit/task_table_test.cpp`：步骤、私有检查点与继续（`TaskTable.decisions_commit_steps_checkpoints_and_progress`）；Task 移动或让位时各自的步骤与检查点不变，新 Task 从空开始（`TaskTable.tasks_keep_their_steps_and_checkpoints_as_places_change`）；`add_step` 拒绝重复、不合法或超出上限的步骤（`TaskTable.add_step_refuses_a_taken_invalid_or_extra_step`）；没有私有检查点的表（`TaskTable.table_without_checkpoints_keeps_empty_ones`）。
- `tests/unit/kernel_api_test.cpp`、`tests/contract/abi_layout.c`、`tests/contract/c_abi_smoke.c`：新容量的范围、默认值与内存增长，0.11 布局的配置取默认值，ABI 0.12 布局，C 调用方的配置。
- 在本地植入 40 个缺陷（检查遗漏或换序、提交遗漏或早于版本上限检查、结束时保留旧检查点、查重按前缀、继续不提升版本或 `commit_seq`、缓冲不复制或不清零、输出容量取 Kernel 的上限、快照缺字段、槽位按下标、`add_step` 不检查、注册不检查或不保存上限等），38 个使测试失败；其余 2 个与原实现行为相同：先查重再检查 `step_key`（决策 5），以及继续之后多重算一次到期工作。
- 调用的步骤（`logical_call_id`、绑定、输入摘要、结果引用、未知状态）、逐条查询步骤、检查点的持久化与重启后的兼容检查不标为已实现。
