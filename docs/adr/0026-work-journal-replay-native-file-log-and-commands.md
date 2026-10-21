# ADR-0026：持久化与恢复（二）：工作日志、重放、Native 文件存储与命令幂等

- **状态**：已接受（`MA-010` 的 B、C、D 部分在 M1 已实现）
- **日期**：2026-10-21
- **关联**：计划 §2.8、§4.2、§4.3、§9.1；`MA-010`；ADR-0016、ADR-0017、ADR-0018、ADR-0019、ADR-0020、ADR-0022、ADR-0024、ADR-0025；C-COMMIT-001 至 C-COMMIT-005、C-COMMIT-016、C-STORE-002、C-STORE-003、C-STORE-007、C-STORE-013、C-STORE-014、C-WIRE-003 至 C-WIRE-005、C-HOST-009、C-HOST-010、C-HOST-015、C-HOST-017 至 C-HOST-019、C-ID-006

## 背景

ADR-0025 把 `MA-010` 拆为 A 至 F 六个部分，A（终态 Goal 让位）已合入。本 ADR 记录合为一个 PR 的 B、C、D。在此之前，transition 只在内存中应用（ADR-0017 决策 3），日志里只有 `BOOT_EPOCH` 批次（ADR-0016），重启后 Agent 实例、Goal、Task、步骤与私有检查点全部丢失；`durable_seq` 在 `RUNNING` 中停在启动批次，从不覆盖已接受的意图；非零 `command_id` 以 `UNSUPPORTED`（`COMMAND_ID_UNSUPPORTED`）拒绝；Native Host 没有存储端口，只有模拟 Host 的日志（在内存中）。

B 部分的两个产品语义问题已由用户选定推荐项（2026-10-20，记于 ADR-0025）：重启后各计时从恢复起按完整时长重新计算；目录项缺失或版本改变时保留对象并使其停下。随后用户对其余问题也选了推荐项：步骤报告带上 `durable_seq`，命令表按 principal 有界并带下限。写入方栅栏、`FENCED` 生命周期值、双槽快照与 schema 0 到 1 的迁移属于 E、F 部分，由下一个 PR 的 ADR 记录。

## 决策

1. **每个 transition 写一个批次，记录写对象的状态。** `RUNNING` 中每个 transition（意图、到期工作、决策、本地输入，以及进入 `RUNNING` 时的启动 transition）在应用的同一步把一个批次编码进存储缓冲区。批次含它改变的每个对象在它之后的状态：`INSTANCE`；新建 Goal 的 `GOAL` 与每个改变的 Goal 的 `GOAL_STATE`；新建 Task 的 `TASK`、本次追加的步骤 `TASK_STEPS`、本次写入的私有检查点 `TASK_CHECKPOINT` 与每个改变的 Task 的 `TASK_STATE`；接受命令时的 `COMMAND_MAPPING`。类型编码 2 至 9，均为版本 1 且置 `REQUIRED`，布局与批次内次序见 `contracts/storage/records.md`。记录是状态而不是操作，所以重放不重新执行规则、不调用策略、不读时钟，只把状态放回表中。新建对象取代的对象记在记录中（`replaced_goal_id`、`replaced_task_id`），重放按记录取代它，并像运行时一样带走让位 Goal 的 Task（ADR-0025 决策 5），不重新选择。记录不含单调时间（C-STORE-014）：Goal 记下 `expiry_after_us` 与 `occurrence_wait_max_us` 这样的时长，Task 的 `due_us` 不写入。不改变持久状态的 transition（`AYNI_INTERNAL` 的情形）写一个空批次，`commit_seq` 因此仍然连续（C-STORE-003）。
2. **日志环与空间。** 已提交、尚未存储的批次按提交次序留在存储缓冲区里，组织为 bip buffer（`durability/journal`）：每个批次连续存放，至多两段，每个批次另占 4 字节。一个 transition 开始之前，环中须有一块能放下任一 transition 最大批次（下称 W，由容量算出）的连续空间，所以编码不会中途失败，transition 也不会只应用一半。没有空间时，`ayni_submit_intent` 在会话与命令检查之后返回 `AYNI_BUSY`（`STORAGE_BACKLOG`）；到期工作、决策与事件留在队列中，`more_ready` 与 `next_deadline_us` 不计它们，追加结果的投递唤醒属主。同一时刻至多一个追加在途；每一步最后，日志为最早的待存批次发起追加，不论剩余预算多少，这次调用不算一项，应用它的结果算一项。W 在默认配置下为 3,696 字节，最小容量下为 624 字节，最大容量下为 85,952 字节。`storage_buffer_bytes` 的默认值改为 0，表示 2 × max(W + 4, 256)，这样一个批次在途时下一个 transition 仍有空间；显式值须不小于 256 且不小于 W + 4，否则 `AYNI_INVALID_ARGUMENT`；上限从 65,536 提高到 1,048,576，以容纳最大容量。
3. **`durable_seq` 推进到已存储的批次，报告在两处（用户选定推荐项）。** 追加结果为 `OK` 时 `durable_seq` 前进到该批次的 `commit_seq`。`AYNI_OK` 仍表示已接受、尚未持久（C-COMMIT-003），调用方以 `durable_seq` 覆盖结果中的 `commit_seq` 判断持久（C-COMMIT-004）。`ayni_kernel_info_v1` 与新增的 `ayni_step_report_v1.durable_seq` 都给出它，属主每一步都能看到，不必另行查询。
4. **写入失败进入 `RESTRICTED`，`DRAINING` 先存完。** 追加结果为 `FAILED`、`UNKNOWN`（或未定义的结果）、或请求被 Host 拒绝时，Kernel 进入 `RESTRICTED`，原因分别为 `STORAGE_WRITE_FAILED`、`STORAGE_WRITE_UNKNOWN`、`STORAGE_REJECTED`。之后的批次永不存储：缺口之后的批次无法按序读出（C-STORE-003）。内存中已应用的状态仍可查询，但不再接受意图、不做到期工作。同一身份的有界重试与受限后的重载随 E 部分。`ayni_begin_shutdown` 之后，`DRAINING` 继续逐个追加 `RUNNING` 提交的批次，在没有请求在途且日志已空或已失败时进入 `STOPPED`。批次放不进给它的空间是本库的缺陷，以新增的 `AYNI_RESTRICTION_INTERNAL`（9）进入 `RESTRICTED`，不掩盖。
5. **重放整批暂存，失败时不留痕迹。** 启动序列把它不拥有的记录按日志次序交给重放，表因此依次经过每个已存储 transition 之后的状态。批次新建的 Goal 或 Task 须在同一批次带有它的状态记录，否则为 `LOG_CORRUPT`；违反表规则的记录也是 `LOG_CORRUPT`，规则只接受本库写得出的变化（`contracts/storage/records.md`：Goal 的 owner 是已启动的实例，终态对象不再改变，版本不减，等等）。日志读完时还须一致：没有未终结的 Task 留在终态 Goal 之下，因为结束 Goal 的 transition 同时结束它的 Task（ADR-0025 决策 5），否则同为 `LOG_CORRUPT`。本次启动的容量或限值放不下日志中的状态时，以新增的 `AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY`（8）进入 `RESTRICTED`：日志由容量更大的配置写入。任何受限都丢弃已重放的全部内容，所以恢复失败的 Kernel 中没有半个日志的状态。重放完成且本次的 `boot_epoch` 已持久后，才按决策 6、7 适配本次启动并进入 `RUNNING`。
6. **计时从恢复起按完整时长重新计算（产品语义，用户 2026-10-20 选定推荐项）。** 单调时钟只在一次启动内有意义，所以以恢复结束时刻 R 为起点：失效时刻为 R + `expiry_after_us`；`INTERVAL` 的下一次 occurrence 为 R + delay（尚未取过 occurrence）或 R + period（已取过）；其他触发为 R + delay；`LOCAL_INPUT` 的 Goal 从 R 起接受输入，等待中的 occurrence 从 R 起计等待上限；恢复的 Task 的 `due_us` 为 R。宁晚不早：断电期间到期的 occurrence 不丢失，也不在恢复时立刻派生，而是从 R 起再等完整时长；失效同样推迟，不提前。日历提醒的“错过的提醒补发一次”（计划 §14.1、C-PROD-018）依据可信的墙上时间，随 `MA-019`，不由单调时钟上的触发实现。
7. **目录项缺失时保留并停下，只在内存中（产品语义，用户 2026-10-20 选定推荐项）。** 目录（Goal 类型、策略、Agent）每次启动重新注册，日志中的对象却跨启动保留。本次没有注册其 Goal 类型（或该版本）的未终结 Goal 停下，`ayni_goal_info_v1.hold_reason` 为 `GOAL_TYPE_UNKNOWN` 或 `GOAL_TYPE_VERSION`（原 `reserved` 字段）：没有到期工作、不接受本地输入，仍可取消。它的 Task 以同样的原因停下（状态分别为 `NOT_FOUND`、`UNSUPPORTED_VERSION`）。Goal 类型本次不再有策略、或私有检查点不是该策略当前版本写的 Task 也停下，状态 `UNSUPPORTED_VERSION`，原因为新增的 `STRATEGY_CHANGED`（C-POLICY-007、C-POLICY-010、C-LIFE-024）；检查点大于该策略 `max_checkpoint_bytes` 的，状态 `CAPACITY_EXCEEDED`，原因 `POLICY_OUTPUT_LIMIT`。每个原因码仍只配一个状态码。本次没有注册的 Agent 的实例为 `FAULTED`，`ayni_agent_info_v1.fault_reason` 为新增的 `AGENT_UNREGISTERED`（原 `reserved` 字段，属于 `NOT_FOUND`）。这些停下与 `FAULTED` 都不改变版本，也不写入日志，日志保留对象原样，重新注册的启动看到的就是日志留下的样子。所有者 `FAULTED` 的 `READY` Task 按既有规则转为 `PAUSED`，所有者恢复后再转回，这一变化是 transition，在启动 transition 中写入日志。
8. **实例跨重启保留，会话不保留。** 日志保留 Agent 实例，`instance_id` 跨重启不变；首次注册的 Agent 在启动 transition 中启动实例。会话依赖调用方的连接，不写入日志，重启后须重新打开；命令按 principal 而不是会话记录，所以换一个会话重试同一命令仍得到原结果。
9. **命令幂等：按 principal 有界，带下限（用户选定推荐项）。** 非零 `command_id` 是会话 principal 的命令。查找在会话检查之后、`BUSY` 之前：同一 principal、同一 `command_id`、内容摘要（FNV-1a，覆盖 kind、`expected_revision` 与正文）相同的已接受命令返回 `AYNI_OK` 与首次结果（`commit_seq` 为首次应用它的 transition，不论在哪次启动），不应用、不调用 wake；摘要不同返回 `AYNI_IDEMPOTENCY_CONFLICT`（`COMMAND_CONFLICT`）；未映射且不大于该 principal 下限的返回 `AYNI_EXPIRED`（`COMMAND_EXPIRED`），因为它可能被接受过。被拒绝的命令不记录，重试时重新判断。映射在应用命令的同一 transition 中以 `COMMAND_MAPPING` 写入，所以与结果一同持久。每个 principal 至多保留 `command_capacity`（新增配置项，默认 32，上限 1,024）个映射；放不下时 `command_id` 最小的让位，并把下限提高到它；记录带有当时的下限，重放取最大值。按递增编号命令的调用方只会失去最早的重试。`COMMAND_ID_UNSUPPORTED` 不再返回。
10. **Native 文件存储。** `ayni::hosts::native::Host::open_storage(directory, request_capacity)` 在状态目录中打开（首次时创建）文件 `commit_log`，以 `flock` 排斥其他写入方，检查全部帧后启动一个 I/O 线程按序服务请求，完成后以 `ayni_completion_post` 投递，之后不再触碰缓冲区与完成对象（C-HOST-019）。文件头为 `AYNL` 与格式版本 1；每帧为 `AYNF`、载荷长度、`commit_seq`、这 16 字节的 CRC-32、载荷与载荷 CRC-32。追加在 `fdatasync` 成功后才为 `OK`（C-HOST-010）。只有末尾一帧可能撕裂：不完整的帧之后没有任何写入（文件在帧内或帧末结束，或帧头不通过且其后再无通过的帧头）时视为撕裂尾部，读取止于它之前，下一次追加截断它（C-HOST-009）；其他不完整的帧是落盘后的损坏，读到它时读取失败，追加也失败，Kernel 进入 `RESTRICTED` 而不丢弃其后的批次（C-STORE-009）。末帧在介质上损坏与撕裂无法区分，按撕裂丢弃。存储未打开时表中没有存储端口，`ayni_start` 返回 `AYNI_UNSUPPORTED`（C-HOST-015）。`native_node` 与 `ayni-sim` 使用它。各 Host 共用一组存储契约测试。
11. **提交后的唤醒规则。** `ayni_submit_intent` 在两种情况下调用 Host 的 wake：它提交了批次且提交前日志空闲（已有批次在等时，那个批次已经唤醒过属主，追加结果的投递还会再唤醒），或结果为 `AYNI_OK` 且产生了到期工作。属主在下一步追加批次并得知下一个期限。
12. **ABI 0.14。** 新增 `ayni_config_v1.command_capacity`、`ayni_step_report_v1.durable_seq`；`ayni_agent_info_v1` 与 `ayni_goal_info_v1` 的 `reserved` 分别改为 `fault_reason` 与 `hold_reason`；新增受限原因 8、9 与失败原因 34 至 38；`storage_buffer_bytes` 的默认值与上限按决策 2 改变；`ayni_step`、`ayni_begin_shutdown`、`ayni_submit_intent` 与查询结构的说明标明自 0.14。按 C-VER-006，0.13 的调用方须重新编译。默认配置的内存需求从 46,432 字节增至 57,944 字节：存储缓冲区从 1,024 字节增至 7,400 字节（2 × 3,700），命令表 2,072 字节（1 个 principal 行，32 个 64 字节的映射），其余不变；最小工作容量从 26,680 字节增至 27,344 字节。

决策 3、6、7、9 是产品语义，已请用户决定，用户都选了推荐项。其余为实施细则，未单独请用户决定。

## 后果

- 重启不再丢失工作：实例、Goal、Task、步骤、私有检查点与命令映射回到最后一个已存储 transition 之后的状态。断电时已提交、尚未存储的 transition 丢失，调用方须以 `durable_seq` 判断哪些结果可依赖。
- 每个 transition 多一次编码与一次 Host 追加；Native Host 每次追加一次 `fdatasync`，吞吐受存储同步速度限制。存储慢时 `AYNI_BUSY`（`STORAGE_BACKLOG`）成为正常的背压信号，调用方须稍后重试；到期工作、决策与事件则延后。
- 默认内存需求增加约 11.5 KB，主要是存储缓冲区。资源紧的配置可显式给出不小于 W + 4 的缓冲区，代价是一个批次在途时下一个 transition 须等待。
- 恢复把每个计时推迟到恢复之后，断电时间越长，到期越晚于原计划；依赖精确时刻的 Goal 须用户自行处理。
- 目录缺失时对象停在内存中而不改变日志，所以固件回退后再升级不会丢失 Goal 或 Task；代价是停下的对象仍占容量，须取消才释放。
- 重放要求本次容量不小于写入日志的配置；缩小容量的固件以 `STATE_EXCEEDS_CAPACITY` 受限，须换回更大的配置或清空数据。
- `command_capacity` 比写入日志的启动大时，重放不再挤掉上一启动已让位的映射，这些命令的重试得到首次结果，与从未让位相同；下限仍取记录中的最大值，其余不大于下限的未映射命令仍为 `EXPIRED`。两种情况都不会重复应用。
- 日志只增不减：快照、轮转与迁移随 F 部分，在此之前恢复时间与日志大小随 transition 数线性增长。写入失败后只能重启，重试与栅栏随 E 部分。
- ESP-IDF Host 仍没有存储端口，`ayni_start` 在其上返回 `AYNI_UNSUPPORTED`；它的存储随实际开发板的工作补上。
- ADR-0022 留给 `MA-010` 的实例状态中，`FAULTED` 由决策 7 使用；策略仍没有初始化，实例不经过 `STARTING`，检查点不兼容时停下的是 Task 而不是实例。

## 备选方案

- **每次 transition 写操作记录，重放时重新执行规则**：记录更小，但重放依赖策略、目录与规则的版本，固件升级后重放结果可能不同。否决。
- **先应用、写满再拒绝**：transition 在编码时才发现空间不足，就要回滚已应用的状态。否决；改为开始前预留最大批次的空间。
- **每个对象各自记录绝对截止时间**：单调时钟跨启动无意义（C-TIME-003），墙上时钟又不可信。否决；用户选了决策 6。
- **恢复时立刻派生断电期间到期的 occurrence**：断电时长未知，单调时钟无法判断哪些已经错过；需要可信的墙上时间。未选（决策 6 的备选项）。
- **丢弃目录缺失的对象，或写入一个终态**：固件回退即永久丢失工作。未选（用户选了决策 7）。
- **命令映射永久保留或按会话保留**：前者无界；后者换会话重试就重复应用。未选（用户选了决策 9）。
- **命令表满时拒绝新命令**：调用方无法自行清理。未选。
- **`durable_seq` 只在 `ayni_get_kernel_info` 中给出**：属主每一步都要多一次查询。未选（用户选了决策 3）。
- **Native 存储每个批次一个文件**：目录项更新需要额外的目录同步，且文件数无界。否决。
- **写入失败后跳过该批次继续**：日志出现缺口，后续批次无法按序读出。否决。

## 验证

- `tests/unit/kernel_recovery_test.cpp`：
  - `WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch` 对一段工作日志的每个已存储前缀重新启动，恢复的实例、Goal、Task、步骤、检查点与命令映射与当时的状态一致；
  - `restart_after_shutdown_keeps_all_the_work`、`append_cut_by_power_loss_counts_only_when_whole`、`power_loss_takes_the_batches_never_appended` 覆盖正常关闭、追加中断电与未追加批次丢失；
  - `timers_restart_from_the_recovery_clock_with_their_full_length`、`input_goal_takes_inputs_after_a_restart` 覆盖决策 6，包括断电 60 个周期后不补建 occurrence；
  - `goal_type_this_boot_lacks_holds_its_goals_and_tasks`、`task_its_strategy_cannot_serve_is_held`、`instance_of_an_agent_no_longer_registered_is_faulted` 覆盖决策 7：策略版本改变、GoalType 不再有策略、检查点超过策略上限三种停下，以及换回原策略的启动从原检查点与步骤继续；
  - `agent_registered_for_the_first_time_starts_after_the_logged_ones` 覆盖决策 8；
  - `log_beyond_the_capacities_restricts_and_shows_nothing`、`log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt` 覆盖决策 5 的两种受限，后者改写一个真实日志的末批次；
  - `WorkCommands.*`（5 个）覆盖决策 9，包括跨重启与更大容量的启动；
  - `WorkJournal.*`（6 个）覆盖决策 2 至 4：没有空间时意图与事件等待、Host 存下批次后 `durable_seq` 才前进、追加失败受限、`DRAINING` 存完或在失败时停止。
- `tests/unit/host_storage_contract_test.cpp`（`HostStorage.*`，12 个，模拟与 Native Host 共用）：读写往返、按 `commit_seq` 起点读取、只返回整批、每个请求恰好完成一次、新 Host 实例保留批次且不投递旧请求、撕裂尾部被丢弃并覆盖、末帧之前的损坏不被丢弃且使 Kernel 受限。
- `tests/unit/native_host_test.cpp`（`NativeHost.*` 新增 10 个）与 `tests/unit/native_node_test.cpp`（`NativeNode.*` 新增 6 个）：端口随存储出现、单一写入方、文件格式与两个 CRC、撕裂尾部截断、损坏日志拒绝追加、关闭时应答全部请求、同一目录的每次启动持久下一个 epoch。
- `tests/unit/kernel_lifecycle_test.cpp`、`tests/unit/kernel_api_test.cpp`、`tests/unit/prefix_scan_test.cpp`：分块读取比缓冲区长的日志、步骤报告按 `struct_size` 给出 `durable_seq`、默认缓冲区容纳两个最大批次、每条记录带有所属批次、没有空间的记录停止扫描。
- 环、命令表、记录编解码与恢复函数各有单元测试：`JournalRing.*`、`CommandTable.*`、`WorkRecords.*`（逐字段布局、截断与多余字节、不含启动内的时间），`AgentRestore.*`、`GoalRestore.*`、`TaskRestore.*`（每种记录只接受本库写得出的变化）与 `WorkReplay.*`（批次内新建与状态的配对、owner 须已启动、日志末尾的一致性），以及 `WorkJournal` 中按重放次序编码、只写改变的对象、最大 transition 恰好填满 W 的三个。
- 植入缺陷共 43 个，41 个被至少一个测试发现，剩下 2 个与原实现行为相同：
  - 日志、提交与 `DRAINING` 的 16 个中 15 个被发现。剩下的是去掉 `DRAINING` 进入 `STOPPED` 的条件中的“日志已空或已失败”：日志有待存批次且未失败时，同一步在检查之前已发起它的追加（或因没有空闲槽位而已有请求在途），所以“没有请求在途”已经蕴含它；保留它作为防御。
  - 环、命令表、记录编解码与恢复函数的 13 个全部被发现。
  - 重放校验（终态对象、停下的 Task、触发游标、实例版本与停止、owner、末尾一致性）的 14 个中 13 个被发现。剩下的是把实例“版本不减”收紧为“版本必须增加”：实例的每次变化都使版本加一，本库写出的日志中同一实例的两条记录版本从不相等，两种写法接受的日志相同；保留与 Goal、Task 一致的“不减”。
