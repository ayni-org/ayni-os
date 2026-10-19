# ADR-0019：工作模型第二部分（二）：延时与周期触发

- **状态**：已接受（`MA-004` 第二部分的第二个 PR 在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §2.1、§2.2、§4.4、§5.3、§14.1；`MA-004`、`MA-010`、`MA-011`、`MA-019`；ADR-0016、ADR-0017、ADR-0018；C-TIME-001、C-TIME-007、C-LIMIT-007、C-PROD-018

## 背景

ADR-0018 把 `MA-004` 第二部分拆为 Task 与传播、时间、事件三个 PR。时间部分包括延时与周期触发、occurrence 去重与补发、失效与有界等待，以及 `next_deadline_us`。它仍然太大，按依赖再拆为两个 PR：

- **触发（本 ADR）**：延时与周期触发、occurrence 编号与合并、派生顺序与 `next_deadline_us`。
- **失效与有界等待**：Goal 的失效时间、occurrence 与 Task 的等待上限（C-RES-005）及其原因。

计划 §4.4 与 C-TIME-007 区分单调间隔（“每两分钟”）与日历规则（“每天 14:00”）。日历规则依赖可信绝对时间、时区与夏令时（C-TIME-004～006），随 `MA-011`；本部分只做 Host 单调时钟上的时间。Goal 与 Task 仍然只在内存中（ADR-0017），重启后的补发与游标持久化随 `MA-010`。

其中两个产品语义问题已请用户决定（决策 3、4）；本 ADR 按推荐项实现，用户改选时修订本 ADR 与相应实现。

## 决策

1. **两种新的触发种类（ABI 0.7）。** `DELAY`（2）：创建后 `trigger_delay_us` 到期一次。`INTERVAL`（3）：创建后 `trigger_delay_us` 第一次到期，此后每隔 `trigger_period_us` 到期一次。`ayni_goal_create_v1` 追加这两个字段：`NONE`、`ONCE` 两者都须为 0，`DELAY` 的周期须为 0，`INTERVAL` 的周期不得小于 `AYNI_TRIGGER_PERIOD_MIN_US`（1 秒）；不符时为 `INVALID_ARGUMENT`，与未知种类一样属于格式错误。`ONCE` 等同于创建时到期的一次 occurrence。超出时钟范围的时间饱和为“永不到期”，不回绕。
2. **occurrence 编号即游标。** 一个 Goal 的 occurrence 按到期顺序从 1 编号，编号在 Goal 的生命期内唯一指向一次 occurrence。游标是已取走的最新编号与下一次到期时间（`ayni_goal_info_v1.occurrences`、`next_due_us`）；`goal_info` 还给出创建时的两个时间。Task 记录它代表的最新编号、代表的个数与该 occurrence 的到期时间（`ayni_task_info_v1.occurrence`、`occurrence_count`、`due_us`，`occurrence_count` 占用原来的 `reserved`）。编号用尽时触发结束，不回绕；1 秒的最短周期使编号在设备寿命内不会用尽。时间放在 `work/goal/trigger_schedule`，由 Goal 表持有游标（ADR-0018 决策 11）。
3. **计时不因暂停而停止（产品语义，推荐项）。** 时间按创建时刻在 Host 单调时钟上推算，Goal、祖先或实例暂停期间照常走。暂停期间到期的 occurrence 留在 Goal 上，放行后派生；这与“错过的提醒默认补发一次”（C-PROD-018）一致，暂停与断电、恢复使用同一规则。
4. **等待的 occurrence 合并为一个 Task，节拍不变（产品语义，推荐项）。** `ayni_step` 派生时取走截至当时已到期的全部 occurrence，生成一个 Task，记下最新一次的编号与到期时间以及代表的个数；下一次 occurrence 仍按原节拍，是当前时刻之后的第一个。例如每 30 分钟一次、从 9:00 开始，9:10 到 10:20 暂停：10:20 补发一次，代表 9:30 与 10:00，下一次在 10:30。这样同一触发窗口只产生一个 Task，等待多久都不会补建多个 Task（C-LIMIT-007）。是否把多次合并呈现为“汇总”由 GoalType 与呈现层决定（`MA-019`）；“跳过”需要先定义何为错过，随失效与有界等待和 `MA-011`。
5. **最早到期的先派生。** 每次派生在放行且已到期的 Goal 中取到期时间最早的，相同时取先创建的。`ONCE` 的到期时间即创建时间，所以 ADR-0018 的创建顺序是它的特例。一次 `ayni_step` 以开始时读取的时钟取 occurrence，同一 step 中的多次派生看到同一时刻。
6. **`next_deadline_us`。** `RUNNING` 中，step 报告放行的 Goal 中最早的下一次到期时间；有可派生的 Goal 时 `more_ready` 为 1；其他状态报告 `UINT64_MAX`。被暂停的 Goal、Task 表没有槽位、ID 用尽时不报告期限：前两种只能由意图解除（恢复、结束 Task），意图会唤醒 owner（ADR-0018 决策 8），ID 只有重启后才再有，因此 Host 不会为等不到的期限反复醒来。创建有 occurrence 的 Goal 也唤醒 owner，Host 由下一次 step 得知新期限。暂停不使任何 Task 可以派生，所以仍不唤醒 owner，但下一次 step 会重新扫描，报告去掉被暂停 Goal 后的期限；Host 手中较早的旧期限至多使它多醒一次。
7. **只在需要时扫描。** 搜索没有找到可派生的 Goal 时，`WorkModel` 记下下一次到期时间；之后的 step 只在意图改变了哪些 Goal 可以派生（包括暂停）、或该时间已到时才扫描 Goal 表。
8. **意图读取时钟。** `ayni_submit_intent` 在应用意图前读取一次 Host 单调时钟，新 Goal 的时间从这一刻起算（C-TIME-008：所有者线程、不阻塞）。

## 后果

- 延时与周期目标在 `RUNNING` 中按时派生 Task；Host 按 `next_deadline_us` 睡眠即可，不需要轮询。
- 暂停的周期目标恢复后补发一次；它在暂停期间不设期限，恢复意图负责唤醒。
- 时间都是 Host 单调时钟上的本机值，可以在本机查询中给出，但不能持久化或跨设备比较（C-TIME-002）；管理 API（`MA-038`）须换算为相对时间，日志记录恢复策略而不是旧的单调值（C-TIME-003，`MA-010`）。
- 周期目标的 Task 仍不会完成（第三部分），所以每个周期都占用一个 Task 槽位，直到 Task 结束让位（ADR-0018 决策 3）；失效与等待上限的 PR 补上超时结束。
- 每次扫描是 Goal 数乘以祖先深度，在已配置上限内；随 M1 的 `step` 时长测量复核。
- 重启仍丢失全部 Goal 与 Task，重启后的补发随 `MA-010`。

## 备选方案

- **暂停时停止计时，恢复后整体推迟**：与已定的“补发一次”规则不一致，且暂停多久提醒就晚多久。推荐计时继续。
- **补发后从补发时刻重新计时**：每次补发都使节拍漂移，多个周期目标之间更难预期。推荐保持原节拍。
- **每个错过的窗口各派生一个 Task**：长时间暂停或 Host 未运行后会一次生成大量 Task，违反 C-LIMIT-007。否决。
- **按创建顺序而非到期时间派生**：先创建、晚到期的目标会在槽位紧张时挤占先到期的目标。否决。
- **由 GoalType 回调计算下一次到期时间**：Kernel 在 step 中调用调用方代码，扫描成本与正确性都依赖外部实现；两种单调触发用类型化字段即可表达。否决，日历规则随 `MA-011` 另行设计。
- **不设最短周期**：微秒级周期会让一个目标在一小时内用尽 occurrence 编号，也不是秒级工作的合理用法（计划 §2.1）。否决，取 1 秒。

## 验证

- `tests/unit/trigger_schedule_test.cpp`（`TriggerSchedule.*`）：各种类接受的时间、第一次到期、超出时钟范围的时间、一次性触发结束、周期按节拍到期、等待的 occurrence 合并且节拍不变、编号用尽时结束。
- `tests/unit/kernel_trigger_test.cpp`（`WorkTrigger.*`）：经 C ABI 的延时与周期目标、`next_deadline_us`、Step 之间错过的 occurrence 合并为一个 Task、暂停的 Goal 与实例恢复后补发一次且暂停期间不设期限、取消的 Goal 不再到期、最早到期的先派生、Task 表满或 ID 用尽时不设期限、时间预算在派生前用尽时仍报告 `more_ready`、`DRAINING` 不报告期限、一次 step 以开始时刻取 occurrence。
- `tests/unit/goal_table_test.cpp`、`tests/unit/kernel_work_test.cpp`、`tests/contract/abi_layout.c`：游标、终态 Goal 不再有下一次 occurrence、格式错误的触发时间、ABI 0.7 布局。
- 失效、等待上限、日历规则与重启后的补发相应条款不标为已实现。
