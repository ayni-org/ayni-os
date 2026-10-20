# ADR-0018：工作模型第二部分（一）：Task、派生与传播

- **状态**：已接受（`MA-004` 第二部分的第一个 PR 在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §2.2、§2.4、§3.2、§4.2、§5.3、§9.1；`MA-004`、`MA-010`、`MA-019`；ADR-0007、ADR-0008、ADR-0016、ADR-0017

## 背景

ADR-0017 把 `MA-004` 拆成三部分，第二部分覆盖 Task、occurrence 与触发、`ayni_post_event`、传播、实例暂停/恢复/停止、失效与有界等待。第二部分本身仍然过大，按依赖再拆为三个 PR：

- **Task 与传播（本 ADR）**：Task 表、从 Goal 派生 Task、Goal 与实例的变化传到 Task、实例意图与 owner 检查。
- **时间**：延时与周期触发、occurrence 去重与补发、失效、有界等待与 `next_deadline_us`。它后来再拆为触发、失效与有界等待两个 PR（ADR-0019）。
- **事件**：`ayni_post_event` 与事件触发。

ADR-0017 曾把“暂停助理作用的集合”列入第二部分；计划 §9.1 把“暂停集合”交给助理协调器（`MA-019`），C-PROD-020 的集合由它选择，本部分只提供它要使用的暂停机制。

文档对 Task 只有一句生命周期描述（`lifecycles.md`），没有值、转换表或传播矩阵，也没有说 GoalType 怎样产生 Task。其中三个产品语义问题与一个实施细则已请用户决定（见决策 3、5、6、8）；本 ADR 按推荐项实现，用户改选时修订本 ADR 与相应实现。

## 决策

1. **Task 表在 `work/task/`（ADR-0008）。** Task 属于创建它的 Goal，owner 继承自 Goal，之后不变（C-LIFE-020）。字段：`task_id`、`goal_id`、owner、occurrence 序号（该 Goal 的第几次触发，从 1 开始）、生命周期、`revision`（创建时为 1，每次改变加 1，不回绕）、`cancel_revision`、创建它的 `transition_id` 与最后一次改变的 `commit_seq`。`task_id` 与其他 Kernel ID 共用同一个带启动代际的分配器（C-ID-010 修订）。
2. **生命周期值与本部分的转换。** 值按 `lifecycles.md` 的描述编号：`READY` 1、`RUNNING` 2、`PAUSED` 3、`CANCELLING` 4、`COMPLETED` 5、`CANCELLED` 6、`FAILED` 7、`EXPIRED` 8，全部发布。本部分只到达 `READY`、`PAUSED`、`CANCELLED`：`RUNNING`、`COMPLETED`、`FAILED` 要等策略推进步骤（第三部分），`CANCELLING` 要等可取消的在途请求，`EXPIRED` 随时间部分。没有 `RUNNING` 时 Task 的 `READY`/`PAUSED` 正好等于决策 5 的放行条件；第三部分里运行中的 Task 要到暂停点才进入 `PAUSED`，所以它仍是生命周期字段，不是派生视图（ADR-0007）。
3. **容量：结束的 Task 在表满时让位（产品语义，推荐项）。** `task_capacity`（1–256，默认 16，计划 §4.2 的“活跃与可恢复的 Task”）。表满时新 Task 取代最早结束的 Task（终态中 `commit_seq` 最小者）；没有已结束的 Task 时不派生。未结束的 Task 永不被挤掉；回收前仍可查询，回收后查询返回 `NOT_FOUND`，长期历史由日志提供（`MA-010`）。ID 不重用，回收的只是槽位（C-ID-001）。Goal 仍按 ADR-0017 计入终态。
4. **触发由 Goal 选择、由 GoalType 限定。** `ayni_goal_create_v1.trigger` 选择触发种类，`ayni_goal_type_v1.trigger_kinds` 是该类型接受的种类集合（`AYNI_TRIGGER_BIT(kind)`），注册时不得为空，也不得含未知种类；目标的种类不在集合中时 `INVALID_ARGUMENT`（`TRIGGER_KIND`）。本部分实现两种：
   - `NONE`（0）：Goal 自己不产生 Task，例如只用来组织子目标的父目标。
   - `ONCE`（1）：Goal 第一次满足放行条件时产生一个 Task。

   时间部分加入延时与周期种类及其参数（ADR-0019），事件部分加入事件种类；GoalType 的校验函数仍只看参数。
5. **放行条件，暂停不级联（产品语义，推荐项）。** 一个 Goal 的 Task 只有在该 Goal 与它的所有祖先都是 `ACTIVE`、owner 实例是 `RUNNING` 时才能前进。ADR-0017 的“暂停不改变子目标”不变：暂停父目标不改写子目标的状态，但子目标的 Task 随之停下，恢复父目标后自动继续，不需要记录暂停原因。据此：
   - **派生**：放行的 Goal 有待派生的 occurrence 时才产生 Task，新 Task 为 `READY`。
   - **传播**：`GOAL_PAUSE`、`GOAL_RESUME`、`INSTANCE_PAUSE`、`INSTANCE_RESUME` 在同一个 transition 中重算受影响的 Task：未结束的 `READY`/`PAUSED` Task 按放行条件取 `READY` 或 `PAUSED`，状态变化的才提升版本。
   - **取消**：`GOAL_CANCEL` 取消目标及其未结束的后代时，在同一个 transition 中取消它们的全部未结束 Task，并记录各自的 `cancel_revision`（C-LIFE-022）。`TASK_CANCEL` 只取消一个 Task，Goal 不变。（自 ADR-0023 起，被取消的是 Goal 最后一个未结束的 Task、Goal 也不会再有 occurrence 时，Goal 随之取消。）
6. **实例意图；停止即取消名下工作（产品语义，推荐项）。** 新增 `INSTANCE_PAUSE`（`RUNNING → PAUSED`）、`INSTANCE_RESUME`（`PAUSED → RUNNING`）与 `INSTANCE_STOP`（`RUNNING`、`PAUSED → STOPPED`），以实例的 `revision` 作为 `expected_revision`。Owner 固定且 `STOPPED` 没有出口，保留名下目标只会留下永远无法推进的工作，所以停止在同一个 transition 中取消该实例全部未结束的 Goal 与 Task。没有在途调用可收敛，本部分不经过 `STOPPING`。
7. **owner 检查。** `GOAL_CREATE` 的 owner 为 `STOPPED` 或 `FAULTED` 时返回 `INVALID_STATE`（`OWNER_STATE`）。`PAUSED` 的实例仍可接收新目标：用户刚下的命令不会丢失，Task 等实例恢复后再派生（C-LIFE-023 “不新增工作”约束的是派生）。
8. **派生在 `ayni_step` 中进行。** 每次派生是一个 transition、一个 step 项：按创建顺序找到第一个放行、有待派生 occurrence、且有 Task 槽位与 ID 的 Goal，推进它的触发游标并创建 Task（批次 B2 的 occurrence 游标与 Task 同属一个 transition；预算预留随 `MA-018`）。这样意图的 transition 只改变它点名的对象，派生单独检查（C-AUTH-004），也与时间部分的定时触发共用同一入口。
   - 只在 Kernel `RUNNING` 时派生；`DRAINING` 与 `RESTRICTED` 不产生新工作。
   - 意图使派生可能到期时（创建 `ONCE` 目标、恢复目标或实例、Task 结束腾出槽位），`ayni_submit_intent` 在返回前调用 Host 的 `wake`；还有可派生的 Goal 时 `more_ready` 为 1。
   - 槽位或 ID 不足时 occurrence 留在 Goal 上等待，不丢弃、不返回给任何调用方；它在何处可见随第三部分的 `blocked_on` 与时间部分的等待上限（C-RES-005）。前面的 Goal 不放行时跳过它，不挡住后面的 Goal。
   - **派生不提升 Goal 的版本（实施细则，推荐项）。** 派生只更新 Goal 的 occurrence 数、最近一个 Task 与 `commit_seq`。`revision` 是意图的 `expected_revision`，用来拒绝基于过时状态的改变（C-COMMIT-012）；派生是 Kernel 自己推进触发进度，不是别人的改变。若派生也提升版本，调用方用 `GOAL_CREATE` 返回的版本暂停或取消目标就会冲突，周期目标的每次触发都会如此。要发现 Goal 的任何变化，比较 `commit_seq`。
9. **修订号上限覆盖整个 transition。** 意图要改变的 Goal、Task 与实例中有版本已是 2^32−1 的，整个意图返回 `CAPACITY_EXCEEDED`（`REVISION_EXHAUSTED`），不改变任何对象（C-COMMIT-012）。派生不改变 Goal 的版本，新 Task 的版本为 1，所以派生不受此限。检查与应用使用同一个判定函数，先完整检查，再应用，不在内存中回滚（C-COMMIT-003）。
10. **ABI 0.6。** `ayni_config_v1` 追加 `task_capacity`；`ayni_goal_type_v1` 追加 `trigger_kinds`；`ayni_goal_create_v1` 追加 `trigger`；`ayni_goal_info_v1` 追加 `trigger`、已派生的 occurrence 数与最近一个 Task 的 ID；`ayni_agent_info_v1` 追加 `revision`。新增 Task 生命周期值、`ayni_task_info_v1`、`ayni_get_task`、`ayni_task_target_v1`、`ayni_instance_target_v1`、四种意图（`TASK_CANCEL` 5、`INSTANCE_PAUSE` 6、`INSTANCE_RESUME` 7、`INSTANCE_STOP` 8，草案中的种类顺延到 9 起）和八个原因码。0.x 中小版本须完全一致（C-VER-006），0.5 新增或改变的结构体仍只接受各自的 `sizeof`。
11. **模块落位（ADR-0008）。**
    - `work/task/task_table`：Task 的生命周期、版本、容量与回收。
    - `work/agent/agent_table`：增加实例的暂停、恢复、停止与版本。
    - `work/goal/goal_table`：增加触发种类与 occurrence 游标。
    - `work/propagation`（`work` 根目录，可以使用各子目录）：放行条件、派生的选择与 Goal/实例变化到 Task 的传播，是跨 Goal、Task 与实例的工作规则。
    - `kernel/intent/`：Task 与实例意图的检查顺序、原因码与对象引用；`WorkModel` 增加 Task 表与派生入口。

## 后果

- Goal 可以产生可查询的 Task，暂停、恢复、取消与停止按上面的规则传到 Task；但 Task 还不会运行或完成，Goal 也还到不了 `SATISFIED`、`FAILED`、`EXPIRED`。
- 一个 `ONCE` 目标只产生一个 Task；`TASK_CANCEL` 之后该目标保持 `ACTIVE` 而不再产生 Task，调用方需要另行取消目标。（ADR-0023 改为随之取消。）
- 槽位不足时派生等待而不报告原因，等待上限与原因码随时间部分与第三部分补齐。
- 停止实例是不可撤销的批量取消；只想临时停下应使用暂停。
- 每次 Goal 或实例变化都要扫描其下 Task（至多 `task_capacity` 个，每个沿至多 `goal_depth_max` 层祖先），派生每次扫描 Goal 表；在已配置上限内可以接受，尚未测量，随 M1 的 `step` 时长测量复核。
- 重启仍丢失全部 Goal 与 Task（ADR-0017），Task 记录与重放随 `MA-010`。

## 备选方案

- **暂停级联到子目标与 Task 的状态**：恢复时无法区分随父目标暂停与单独暂停，需要记录原因。推荐用放行条件代替。
- **意图的 transition 里直接派生 Task**：创建或恢复一个目标就要在同一 transition 中派生其下全部 Task，transition 大小取决于树的规模；派生也就无法与定时触发共用入口。否决。
- **由 GoalType 的回调把参数映射为触发**：Kernel 要多调用一次调用方代码，而触发种类仍需 Kernel 能理解的类型。否决，改为类型化字段加类型声明的集合。
- **派生也提升 Goal 的版本**：版本反映 Goal 记录的每次变化，但持有旧版本的暂停与取消在每次派生后都要重读，`commit_seq` 已能表示记录变化。推荐不提升。
- **结束的 Task 一直占位，或结束即回收**：前者让周期目标最终耗尽容量，后者让刚结束的结果无法查询。推荐表满时回收最早结束的。
- **停止实例时保留名下目标，或要求先清空**：前者留下无法推进的目标，后者把一次停止拆成许多意图。推荐一并取消。
- **为 Task 意图提供暂停/恢复**：计划只要求取消 Goal/Task；单个 Task 的暂停随第三部分的暂停点再定。

## 验证

- `tests/unit/task_table_test.cpp`（`TaskTable.*`）：版本与取消版本、允许的转换、表满时回收最早结束的 Task 而不挤掉未结束的、版本上限。
- `tests/unit/agent_table_test.cpp`：实例暂停、恢复、停止的允许转换与版本。
- `tests/unit/kernel_task_test.cpp`（`WorkTask.*`）：经 C ABI 的派生（`ONCE` 只派生一次、`NONE` 不派生、`ayni_step` 每次派生一项、`more_ready` 与 `wake`）、放行条件（暂停父目标停下子目标的 Task、恢复后继续；Goal 与 owner 须同时放行；不放行的 Goal 不挡住后面的 Goal）、派生不提升 Goal 版本、Goal 与实例变化传到 Task、取消级联到 Task、`TASK_CANCEL`、实例停止取消名下工作、owner 检查、版本冲突与版本上限（含恢复时的 Task 版本上限）、容量与回收、`DRAINING` 不派生、各函数在六个状态下的返回值。
- 未实现的部分（运行、完成、失效、时间触发、事件触发、等待原因）相应条款不标为已实现。
