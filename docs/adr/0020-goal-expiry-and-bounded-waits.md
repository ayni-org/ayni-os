# ADR-0020：工作模型第二部分（三）：Goal 失效与有界等待

- **状态**：已接受（`MA-004` 第二部分的第三个 PR 在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §2.2、§2.4、§4.4、§5.3；`MA-004`、`MA-010`、`MA-018`、`MA-019`；ADR-0007、ADR-0017、ADR-0018、ADR-0019；C-RES-005、C-TIME-001、C-COMMIT-012、C-LIFE-022、C-POLICY-009、C-HOST-021、C-PROD-018

## 背景

ADR-0019 把 `MA-004` 第二部分的时间部分拆为触发、失效与有界等待两个 PR，本 ADR 是第二个。原语要求 Goal 带失效规则（`contracts/domain/primitives.md` 的 `expiry`）；C-RES-005 要求每类工作声明等待上限与超限后的处理及原因码，等待超限不得静默保留在队列中。而按 ADR-0018、ADR-0019，Task 表没有槽位或 ID 用尽时，occurrence 在 Goal 上无限等待，不设期限，也没有可查的原因。

本 ADR 只处理两件事：Goal 在相对创建时间的时刻失效，以及到期的 occurrence 等待 Task 槽位或 ID 的上限。Task 执行中的等待（模型槽、资源、预算，C-RES-004）、“延后”“暂停”两种超限处理与 `blocked_on` 随第三部分与 `MA-018`；日历期限随 `MA-011`。Goal 与 Task 仍只在内存中，失效时刻的恢复策略随 `MA-010`（C-TIME-003）。

其中两个产品语义问题已请用户决定（决策 3、5）；本 ADR 按推荐项实现，用户改选时修订本 ADR 与相应实现。（2026-10-20 用户确认了这些推荐项。）

## 决策

1. **失效时间（ABI 0.8）。** `ayni_goal_create_v1` 追加 `expiry_us`：从创建到失效的时长，0 表示不失效，超出时钟范围饱和为不失效。Goal 记下 Host 单调时钟上的失效时刻，`ayni_goal_info_v1.expires_us` 给出它；没有失效时间、失效被放弃（决策 8）或 Goal 已是终态时为 `UINT64_MAX`。到时后由 `ayni_step` 执行失效，作为一个 item 与一个 transition：Goal 成为 `EXPIRED`，版本加 1，不记 `cancel_revision`。
2. **暂停不挡失效。** 失效是时间条件，不是 Task 的推进；Goal、祖先或 owner 实例暂停时，Goal 仍按时失效。这与 ADR-0019 决策 3“计时不因暂停而停止”一致。
3. **失效级联（产品语义，推荐项）。** Goal 失效时，同一 transition 中它全部未终结的后代也成为 `EXPIRED`，它们未结束的 Task（`READY`、`PAUSED`）成为 `EXPIRED`（Task 生命周期 8），各自版本加 1；已终结的后代与已结束的 Task 不变。规则与取消级联（C-LIFE-022）相同，只是终态不同。后代自己的失效时间较早时，只结束它自己。
4. **失效先于同一 step 中到期的 occurrence。** 一次 step 先执行已到时的失效，再派生，再跳过。Goal 在同一 step 中既到了失效时间、又有已到期而未取走的 occurrence 时，先失效，不再为它派生 Task；更早的 step 已派生的 Task 随 Goal 一起失效。失效释放的 Task 槽位在同一 step 中就可给等待的 occurrence 使用。
5. **等待上限由 GoalType 声明，超限跳过（产品语义，推荐项）。** `ayni_goal_type_v1` 追加 `occurrence_wait_max_us`：Task 表没有槽位或本次启动没有 ID 时，放行 Goal 的已到期 occurrence 至多等这么久，从它到期起算。之后发现等待已超限的 step 取走该 Goal 截至 step 开始已到期的全部 occurrence，不派生 Task，计入 `ayni_goal_info_v1.occurrences_skipped`，并在 `skip_reason` 记下原因：Task 表没有槽位为 `AYNI_REASON_TASK_CAPACITY`（新原因码 27，只用于 Goal 信息），否则为 `AYNI_REASON_ID_EXHAUSTED`。0 表示没有槽位时立即跳过；`AYNI_OCCURRENCE_WAIT_UNBOUNDED`（`UINT64_MAX`）表示一直等，不跳过。按 0 初始化的 GoalType 得到 0，即不等待；需要等待的类型（如普通提醒，`MA-019`）须显式声明。跳过同样推进 occurrence 编号（`occurrences` 包括跳过的），下一次仍按原节拍。（自 ADR-0023 起，跳过后不会再有 occurrence、也没有未结束的 Task 的 Goal 在同一 transition 中进入 `FAILED`。）
6. **跳过不提升版本。** 与派生一样，跳过只改触发游标、跳过计数与原因和 `commit_seq`，调用方持有的版本仍可用于暂停或取消，理由同 ADR-0018：Kernel 自身的推进不应使调用方的意图过期。（自 ADR-0023 起，跳过使 Goal 失败时，失败提升 Goal 与后代的版本，这时 Goal 已是终态。）
7. **等待只对放行的 Goal 计时，从到期起算。** 被暂停的 Goal（或其祖先、owner 实例暂停）不推进，不会因槽位不足被跳过，其 occurrence 在 Goal 上保留，暂停期间不设期限。等待从 occurrence 到期起算，暂停期间也计入，不另记恢复时刻；所以恢复时若仍没有槽位且等待已超限，下一次 step 立即跳过。
8. **失效会使版本越过最大值时放弃失效。** 失效要提升 Goal、其后代或它们的 Task 的版本，其中有已是 2^32−1 的，失效就不能执行（C-COMMIT-012）。Kernel 不能像拒绝意图那样把错误交给调用方，版本也不会降低，重试永远不会成功；因此 step 放弃该 Goal 的失效：生命周期与版本不变，`expires_us` 变为 `UINT64_MAX`，`commit_seq` 更新，算一个 item 与一个 transition。
9. **等待原因是查询时派生的视图。** `ayni_goal_info_v1.wait_reason`：Goal 还有下一次 occurrence 时，说明现在派生会被什么挡住，Task 表没有槽位为 `TASK_CAPACITY`，否则没有 ID 为 `ID_EXHAUSTED`，否则为 `NONE`；没有下一次时为 `NONE`。它在查询时从 Task 表与 ID 分配器读出，不存储，查询不读时钟；暂停挡住的情形由生命周期表达，不在此重复。这与 ADR-0007“等待原因由待决状态派生”一致。
10. **到期工作与 `next_deadline_us`。** step 按顺序找下一项到期工作，相同时取先创建的 Goal：最早到时的失效；否则在有槽位且有 ID 时，放行 Goal 中最早到期的 occurrence（ADR-0019 决策 5）；否则放行 Goal 中等待最早结束的跳过。`next_deadline_us` 取失效时刻与后两者之一的最小值，无界等待不设期限。于是 ADR-0019 决策 6 中“Task 表满、ID 用尽时不设期限”只对无界等待的 GoalType 仍然成立：有界等待在等待结束时设期限，失效不论槽位与 ID 都设期限。创建带失效时间的 Goal 也唤醒 owner。槽位或 ID 减少只会使期限推后或不变，所以仍不唤醒；`WorkModel` 记下上次扫描时是否还有 ID，ID 用尽后的下一次 step 重新扫描。

## 后果

- C-RES-005 在 occurrence 这一层实现：每类 Goal 声明等待上限，超限的 occurrence 带原因计数，不再在 Goal 上无期限、无原因地等待；无界等待是 GoalType 的显式选择。
- 按 0 初始化的 GoalType 在槽位不足时立即跳过。`MA-019` 的普通提醒须声明无界或足够长的等待，默认的补发一次（C-PROD-018）才不受容量影响；即使跳过，也有计数与原因可查，不是静默丢弃。
- 周期目标的 Task 仍不会完成（第三部分），所以 Task 表满后，等待有界的周期目标会按节拍跳过，Host 按期限醒来执行跳过。
- 失效时刻是本机单调值，持久化时须换为恢复策略（C-TIME-003，`MA-010`）。（自 ADR-0026 起，Goal 记录写入 `expiry_after_us`，恢复时失效时刻为恢复结束时刻加上它。）
- 每次扫描仍是 Goal 数乘以祖先深度，另加一次 Task 表扫描判断是否有槽位；随 M1 的 `step` 时长测量复核。
- 0.7 的调用方须重新编译：主版本 0 下不同 minor 互不接受（C-VER-006），`ayni_goal_type_v1`、`ayni_goal_create_v1`、`ayni_goal_info_v1` 只接受 0.8 的大小。

## 备选方案

- **失效只结束该 Goal，后代与 Task 各自保留**：父目标失效后其下的工作仍在推进，与“子目标服务于父目标”不符，用户还要逐个清理。推荐级联。
- **失效只结束 Goal，已派生的 Task 继续**：Task 只在 Goal 放行时推进（ADR-0018），终态 Goal 下留下 `READY` Task 与放行条件矛盾。否决。
- **暂停的 Goal 不失效，恢复后再判断**：暂停会无限延长有期限的工作，与计时不因暂停而停止不一致。否决。
- **一直等，不设上限**：即 ADR-0018、ADR-0019 的行为，违反 C-RES-005“等待超限不得静默保留”。保留为 GoalType 可选的无界等待，不作默认。
- **超时后暂停 Goal**：要由调用方发意图恢复，并把容量问题变成 Goal 的生命周期改变，提升版本使调用方持有的版本过期；跳过不改生命周期，又有计数与原因可查。未选；暂停作为超限处理在第三部分的 Task 层面再议。
- **由 Kernel 规定统一的等待上限**：各 GoalType 的时效差别很大，C-POLICY-009 要求等待上限由 GoalType 声明。否决。
- **跳过时提升 Goal 版本**：调用方持有的版本会因 Kernel 自身的推进而过期。否决。
- **失效遇到最大版本时保留失效时间、每次 step 重试**：版本不会降低，重试永远失败，每次扫描还要算它。否决。
- **在 Goal 上存储等待原因**：每次槽位或 ID 变化都要更新全部 Goal，成本高，且容易与实际状态不一致。否决，查询时派生。
- **等待从恢复时刻起算**：要为每个 Goal 记录恢复时刻，暂停与断电的处理也不再一致。未选；若认为恢复后不应立即跳过，可另行调整。

## 验证

- `tests/unit/kernel_expiry_test.cpp`（`WorkExpiry.*`、`WorkWait.*`）：Goal 按时失效且创建时唤醒 owner；没有或超出时钟范围的失效时间不失效、不唤醒；失效级联到未终结的后代与它们的 `READY`、`PAUSED` Task，已终结的不变；后代较早的失效只结束它自己；暂停的 Goal 与实例暂停的 Goal 仍失效；失效先于同一 step 中到期的 occurrence；失效计入期限；失效不需要槽位与 ID；失效释放的槽位在同一 step 中派生等待的 occurrence；会使版本越过最大值的失效被放弃；槽位不足时超限的 occurrence 被跳过、计数、记原因且版本不变；ID 用尽时以 `ID_EXHAUSTED` 跳过；等待上限 0 立即跳过；无界等待不跳过、不设期限；暂停的 Goal 不跳过、恢复后立即跳过；等待最早结束的先跳过，期限取最早结束的有界等待，不取更早到期的无界等待；时钟到最大值时没有到期工作；`wait_reason` 的派生规则。
- `tests/unit/goal_table_test.cpp`、`tests/unit/task_table_test.cpp`：Goal 失效的级联、失效时刻的计算与饱和、跳过与放弃失效不改版本、失效时的版本检查、Task 从 `READY` 与 `PAUSED` 失效。
- `tests/unit/kernel_work_test.cpp`、`tests/contract/abi_layout.c`：0.7 布局的 `ayni_goal_type_v1`、`ayni_goal_create_v1`、`ayni_goal_info_v1` 被拒绝，ABI 0.8 布局。
- Task 执行中的等待、延后与暂停两种超限处理、失效时刻的恢复策略不标为已实现。
