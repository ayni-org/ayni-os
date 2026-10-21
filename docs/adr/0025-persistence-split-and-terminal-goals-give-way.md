# ADR-0025：持久化与恢复（一）：拆分与终态 Goal 让位

- **状态**：已接受（`MA-010` 的第一个 PR 在 M1 已实现）
- **日期**：2026-10-20
- **关联**：计划 §2.8、§4.2、§9.1；`MA-010`；ADR-0016、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0022、ADR-0023；C-ID-001、C-ID-010、C-LIMIT-007、C-LIFE-022、C-VER-006

## 背景

`MA-010` 的完成定义（计划 §9.1）包括 §2.8 全部批次的断电测试、写入 `UNKNOWN` 或 `FAILED` 与在途动作并存时没有新副作用且可对账、重载后旧写才完成时恢复状态与日志不分叉，以及旧格式与模拟回滚安全。现有代码只写 `BOOT_EPOCH` 记录（ADR-0016）：Agent 实例、Goal、Task、步骤与私有检查点都只在内存中，每次启动从头开始；Native 与 ESP-IDF Host 还没有存储，模拟 Host 的日志在内存中。调用身份、动作账本、Grant、执行许可与预算的记录要到 `MA-006`、`MA-008`、`MA-009`、`MA-011`、`MA-018` 才有，而这些包都依赖 `MA-010`。

Goal 表从不移除终态 Goal，`goal_capacity` 计入它们（ADR-0017 决策 5），回收推迟到 `MA-010`。没有持久化时每次重启清空 Goal 表；持久化之后它不会再自己清空，设备累计创建 `goal_capacity` 个 Goal 后就永远拒绝新 Goal。Task 表已经按“最早结束的先让位”回收（ADR-0018 决策 3）。所以 Goal 的回收先于 Goal 记录落地。

## 决策

1. **拆分为六个部分，分三个 PR 提交。** A 是终态 Goal 让位（本 ADR）；B 是事务日志、提交协调与整批暂存的重放，写下 Agent 实例、Goal、Task、步骤与私有检查点的记录，重启后重新计时，运行中写入失败进入 `RESTRICTED`，模拟 Host 注入断电与短写；C 是 Native 文件存储 Host 与各 Host 共用的契约测试；D 是命令幂等；E 是同一身份的有界重试、写入方栅栏与受限后的重载；F 是快照、日志轮转、存储格式迁移与模拟回滚。顺序为 A、B，然后 C 与 D 并行，再 E、F：B 写下的 Goal 表要有界，C 与 D 都建立在 B 的提交协调上，E 与 F 改变 B 的写入与恢复路径。为加快进度（用户 2026-10-20 要求），A 单独提交，B、C、D 合为一个 PR，E、F 合为一个 PR；各部分的内容与次序不变。（自 ADR-0027 起，E、F 的实现由同一 PR 补入；写入故障经同一身份有界重试、`FENCED` 隔离与重载处理，快照和迁移的切换规则见该 ADR。）
2. **其余断电验收随各包补齐（产品范围，用户选定推荐项）。** `MA-010` 实现日志机制，并为现有对象写下记录，即算完成。§2.8 中调用、动作账本、Grant、执行许可与预算的批次，以及写入 `UNKNOWN` 或 `FAILED` 与在途动作并存的验收，随这些记录所属的包与 E2（`MA-022`）补齐，各包用 `MA-010` 的断电注入验证自己的批次。否则 `MA-010` 要等依赖它的包完成，或者现在就为它们写只给测试用、以后多半要改的记录。
3. **终态 Goal 在表满时让位（产品语义，用户选定推荐项）。** 规则与 Task 表一致：Goal 表满时，新 Goal 取代最早结束的终态 Goal；没有终态 Goal 时仍以 `CAPACITY_EXCEEDED`（`GOAL_CAPACITY`）拒绝。未终结的 Goal 永不被挤掉，暂停的也不会。`goal_capacity` 仍计入终态 Goal，表未满时不回收，所以刚结束的 Goal 在表满之前一直可查。让位之后，`ayni_get_goal` 返回 `NOT_FOUND`，以它为目标的意图返回 `NOT_FOUND`（`GOAL_UNKNOWN`），以它为父目标的 `GOAL_CREATE` 返回 `NOT_FOUND`（`PARENT_UNKNOWN`）。
4. **让位的次序。** 终态 Goal 不再改变，所以 `commit_seq` 最小的最早结束。未终结的 Goal 没有终态祖先，所以后代不会晚于祖先结束。在同一 transition 中结束的 Goal，先让深度最大的，再让 Goal ID 最小的（先创建的）。这样表中每个 Goal 的父目标都还在表中：父目标若已终结，它的子目标结束得不比它晚，会先于它让位。让位者一次扫描即可找到，代价是 Goal 数。
5. **它的 Task 一起移除。** Goal 结束时，同一 transition 也结束它能到达的全部 Task（ADR-0018、ADR-0022），终态 Goal 也不再派生 Task，所以让位的 Goal 的 Task 都已结束。它们与 Goal 在同一 transition 中离开 Task 表，之后 `ayni_get_task` 返回 `NOT_FOUND`，Task 不会指向一个已换给别的 Goal 的位置。Task 表因此不再只在表满时腾出位置：新 Task 在表满时取最早结束的 Task 的槽位，否则取没有 Task 占用的最小槽位（按 `AYNI_TASK_CAPACITY_MAX` 位标记，栈上 32 字节）。其余 Task 的位置、步骤与私有检查点不动。
6. **位置不再是创建顺序。** 新 Goal 占用让位者的位置与参数块，其他 Goal 不移动，所以一个 Goal 留在表中期间下标不变，Task 与子目标记下的下标都不用改写。代价是子目标可能排在父目标之前。取消、失效与结束的级联、版本上限检查与子目标计数因此改为遍历整张表，而不是从目标之后扫描，代价仍是 Goal 数。子树判定沿父目标上溯，与位置无关。到期工作在时间相同时仍取先创建的 Goal（ADR-0019 决策 5、ADR-0020 决策 10），但改为比较 Goal ID 而不是下标：一次启动内 ID 递增，后来的启动以更大的 `boot_epoch` 为前缀（C-ID-010）。
7. **不留 tombstone。** Kernel 分配的 ID 不复用（C-ID-001、C-ID-010），旧回执与旧意图不会作用到另一个 Goal 上，所以让位的 Goal 不另留删除标记。查询不区分“已让位”与“从未存在”，二者都是 `NOT_FOUND`。C-ID-001 据此注明：Kernel 分配的 ID 由分配器保证不复用。需要 tombstone 的是别的记录依赖被删内容的删除，例如记忆的删除与派生摘要的失效（C-PROD-031），随 `MA-011`。长期历史由日志提供（ADR-0018 决策 3）。（自 ADR-0027 起，用户选择历史保留 A：保留当前可查询对象与既有幂等窗口，快照激活后可清理已覆盖日志，不额外归档已让位对象的长期历史。）
8. **ABI 0.13。** 布局不变，只改语义：`GOAL_CREATE` 在表满时可能取代终态 Goal，被取代的 Goal 与它的 Task 此后 `NOT_FOUND`，`GOAL_CAPACITY` 只在没有终态 Goal 时返回。按 C-VER-006，0.12 的调用方须重新编译。`ayni.h` 中工作模型容量、`GOAL_CAPACITY`、`GOAL_CREATE` 与 `ayni_get_task` 的说明标明自 0.13。Kernel 的内存需求不变：默认配置 46,432 字节，最小工作容量 26,680 字节。

决策 2、3 已请用户决定，用户都选了推荐项（2026-10-20）。B 部分的两个产品语义问题也由用户选定了推荐项：重启后各计时从恢复起按完整时长重新计算，宁晚不早；目录项缺失或版本改变时保留对象并使其停下、记下原因，不丢弃。二者随 B 部分的 ADR 记录。其余为实施细则，未单独请用户决定。

## 后果

- 长期运行的设备不再因累计创建 `goal_capacity` 个 Goal 而永远拒绝新 Goal，只有未终结的 Goal 占满表时才拒绝。Goal 记录落地（B 部分）之后，这也是重启后 Goal 表的上限。
- 表满以后，终态 Goal 与它的 Task 只保留到下一个新 Goal 需要位置为止。需要结果的调用方须在让位之前读取；依赖“终态 Goal 一直可查”或“表满即拒绝”的调用方须调整。
- 子目标可能排在父目标之前，所以 Goal 表的代码不再依赖位置顺序；之后按位置遍历的代码（例如 B 部分的记录与重放）也不能假定父目标在前。
- 表满时每次创建 Goal 多扫描两次 Goal 表（准入时判断有没有可让位的 Goal，插入时找出它），并扫描一次 Task 表移除让位者的 Task；表未满时每次派生 Task 多扫描一次 Task 表找空闲槽位。都在 `AYNI_GOAL_CAPACITY_MAX` 与 `AYNI_TASK_CAPACITY_MAX` 之内，尚未测量。
- 让位依赖“终态 Goal 没有未结束的 Task”（决策 5）。`MA-008`、`MA-018` 使 Task 进入 `RUNNING`、`CANCELLING` 之后，Goal 可能先于它的 Task 终结，届时仍有未结束 Task 的终态 Goal 须不让位，由那时的包保证并补测试。
- 调用、账本、Grant、许可与预算批次的断电验收不在 `MA-010` 中完成，各包须各自补上（决策 2），计划 §9.1 中这些包与 `MA-022` 的完成定义已写明。

## 备选方案

- **终态 Goal 一直保留**：设备累计创建满容量个 Goal 后永远拒绝新 Goal，只能清空数据。未选（用户选了决策 3）。
- **显式清理**：新增清理意图，由用户或 Agent 自己管理空间，不清理就被拒绝；多一个意图种类和它的授权。未选（用户选了决策 3）。
- **等 Goal 的 Task 都离开 Task 表再让位**：Task 表不满时已结束的 Task 不会让位，它们的 Goal 就永远不能让位。否决。
- **移除时压缩，保持创建顺序**：后面的 Goal 与参数块整体前移，并改写全部子目标与 Task 记下的下标。要跨两张表改写下标，代价与出错面都更大。否决。
- **让位时连同全部终态后代一起移除**：一次腾出多个位置，却让还不需要让位的 Goal 提前 `NOT_FOUND`。未选。
- **保留 tombstone，区分已让位与不存在**：需要另一块有界存储，满了以后仍要遗忘；ID 不复用已防止旧回执作用到新 Goal 上。未选。
- **移除 Task 后搬动槽位，保持“表未满时槽位为 0 到 size−1”**：每移除一个 Task 都要复制步骤与私有检查点块，最大配置下约 6 KB；插入时找空闲槽位只需 32 字节的栈。未选。
- **等待调用、账本等记录所属的包完成后再算 `MA-010` 完成**：`MA-010` 与依赖它的包互相等待。未选（决策 2 的备选项）。

## 验证

- `tests/unit/goal_table_test.cpp`（`GoalTable.*`）：表满时才让位，让位的是最早结束而不是最早创建的终态 Goal，新 Goal 取得它的位置与参数块，其他 Goal 的参数不变，全是未终结 Goal 的表拒绝新 Goal（`GoalTable.terminal_goal_that_ended_first_gives_way`）；同时结束的 Goal 先让深度大的，再让先创建的（`GoalTable.goals_ended_together_give_way_deepest_first`）；排在父目标之前的子目标仍在父目标的子树中，计入子目标数，版本上限检查与取消都到达它（`GoalTable.child_in_an_earlier_place_is_still_in_its_parents_tree`）；跳过准入的插入不越界，也不让新 Goal 占用它终态父目标的位置（`GoalTable.insert_into_a_full_table_changes_nothing`）。
- `tests/unit/task_table_test.cpp`：让位的 Goal 带走它的全部 Task，包括移除时被移入别处的；其余 Task 保留步骤与私有检查点；新 Task 取空闲槽位，不取别的 Task 的（`TaskTable.goal_giving_way_takes_its_tasks_and_frees_their_slots`）。
- `tests/unit/kernel_task_test.cpp`（经 C ABI）：表满时最早结束的 Goal 让位，它的 Task 随之 `NOT_FOUND`；查询、`GOAL_CANCEL`、`TASK_CANCEL` 与以它为父目标的创建都得到 `NOT_FOUND`；未终结 Goal 占满的表拒绝新 Goal（`WorkTask.goal_that_ended_first_gives_way_with_its_tasks`）。排在父目标之前的子目标与它的 Task 随父目标暂停、恢复与取消（`WorkTask.child_in_an_earlier_place_follows_its_parent`）。
- `tests/unit/kernel_trigger_test.cpp`、`tests/unit/kernel_expiry_test.cpp`：后创建的 Goal 占了更前的位置时，同时到期的派生与失效仍按创建顺序（`WorkTrigger.tie_goes_to_the_goal_created_first_whatever_its_place`、`WorkExpiry.equal_expiries_go_in_creation_order_whatever_the_places`）。
- `tests/unit/kernel_work_test.cpp`：暂停的 Goal 也占位置，未终结 Goal 占满的表拒绝新 Goal，不取 ID 与 transition（`WorkModel.full_goal_table_refuses_without_side_effects`）。
- 植入 20 个缺陷，18 个被至少一个测试发现。剩下 2 个与原实现行为相同：`descends_from` 不排除目标自身（目标自身在循环之前已检查过版本，或已经终结），以及平局比较以 `time_us != no_time` 代替 `best != no_index` 作前提（二者总是同真同假）。
