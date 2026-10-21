# 持久记录格式

- **关联**：计划 §2.8、§4.2「持久化」、§4.3、§4.4「升级」；ADR-0002、ADR-0016、ADR-0026、ADR-0027；`MA-010`；E2 F0–F8、F11、F12，E4 步骤 6
- **属主**：`durability`（批次编码、持久前缀、快照、重放、栅栏记录、迁移机制）；领域记录的内容由各属主模块定义，`durability` 不理解其语义
- **状态**：M1 当前写 `storage_schema=1`，明确读取 0、1；0 为迁移来源。布局以本文件和编解码测试为准，迁移切换见 ADR-0027；Native/模拟证据不代表 ESP32 或真实 OTA 验证

## 编码约定

- 全部整数为定长小端序；字节串以 `uint32` 长度前缀；不持久化 C++ 内存布局（C-VER-023）。
- 校验和为 CRC-32/ISO-HDLC（与 ESP32 ROM 的 `esp_rom_crc32_le` 相同，Native 由 `foundation` 实现同一算法）。选它是因为 ESP32 可直接用 ROM 实现，不增加固件体积；CRC 只用于发现损坏，不提供授权或防篡改（C-STORE-008）。

## 日志批次

一个提交批次是一个 transition 的全部记录（`domain/commit_and_output.md`）。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYJB` |
| 4 | 2 | `storage_schema` | 存储格式版本 |
| 6 | 2 | `header_length` | 本头部长度（当前 44），含末尾的 `header_crc`；追加的头部字段插在 `header_crc` 之前 |
| 8 | 4 | `writer_epoch` | 写入方代际（C-STORE-005） |
| 12 | 4 | `flags` | 保留，写 0；读到未知位时进入 `RESTRICTED` |
| 16 | 8 | `commit_seq` | 连续递增 |
| 24 | 8 | `transition_id` | 重试同一 transition 时不变（C-COMMIT-008） |
| 32 | 4 | `record_count` | 记录数 |
| 36 | 4 | `payload_length` | 记录区字节数 |
| 40 | 4 | `header_crc` | 始终是头部最后 4 字节（偏移 `header_length − 4`），覆盖偏移 0 至 `header_length − 5`，追加字段因此同样受保护 |
| 44 | `payload_length` | 记录区 | 见下表 |
| — | 4 | `commit_magic` | ASCII `AYJC` |
| — | 4 | `commit_seq_low` | `commit_seq` 的低 32 位，把提交标记绑定到本批次 |
| — | 4 | `payload_crc` | 覆盖记录区 |

每条记录：

| 长度 | 字段 | 说明 |
|---:|---|---|
| 2 | `record_type` | 见“记录类型” |
| 2 | `record_version` | 该类型的版本 |
| 4 | `record_flags` | 位 0 = `REQUIRED`：读不懂时必须进入 `RESTRICTED`；其余位保留，写 0，读到未知位时进入 `RESTRICTED` |
| 4 | `length` | 记录体字节数 |
| `length` | 记录体 | 由属主模块定义的字段编码 |

## 记录类型

名称、所属批次与属主在此固定；类型编码在实现该记录的工作包中分配，记入下表。

| 记录 | 属主 | 所属批次 |
|---|---|---|
| `BOOT_EPOCH`（类型 1，版本 1，置 `REQUIRED`；正文为 8 字节小端 `boot_epoch`） | `kernel` | 单独批次，开放输出前提交（C-ID-009、ADR-0016） |
| `WRITER_FENCE`（类型 10，版本 1，REQUIRED；正文 4 字节小端新 writer epoch，须与头部相等） | `durability` | 单独批次（C-STORE-005） |
| `INSTANCE`（类型 2） | `work/agent` | 启动或改变实例的 transition 批次（ADR-0026） |
| `GOAL`（类型 3）、`GOAL_STATE`（类型 4） | `work/goal` | `GOAL` 在创建它的批次（B1）；`GOAL_STATE` 在每个改变该 Goal 的批次，含 occurrence 游标（B2），所以不另设 `OCCURRENCE_CURSOR` |
| `TASK`（类型 5）、`TASK_STATE`（类型 6） | `work/task` | `TASK` 在派生它的批次（B2）；`TASK_STATE` 在每个改变该 Task 的批次，含 `TASK_PROGRESS` 的内容 |
| `TASK_STEPS`（类型 7）、`TASK_CHECKPOINT`（类型 8） | `work/task` | 提交步骤或私有检查点的决策批次（B3），即 `STEP_CHECKPOINT` |
| `COMMAND_MAPPING`（类型 9） | `kernel` | 接受该命令的 transition 批次（B1） |
| `BUDGET_RESERVATION`、`BUDGET_SETTLEMENT` | `budget` | B2、B4 |
| `CALL_IDENTITY` | `work/call` | B3 |
| `ACTION_STATE` | `work/action` | 准备、屏障与终态各自的批次；终态在 B4 |
| `CALL_RESULT` | `work/call` | B4 |
| `MEMORY_RECORD`、`MEMORY_TOMBSTONE`、`LINEAGE` | `knowledge` | B4、B5 |
| `GRANT`、`GRANT_REVOCATION` | `authority/grant` | 授权变更批次、B5 |
| `INVALIDATION_CURSOR` | `knowledge/lineage` | B5 |
| `SNAPSHOT_COMMIT`（类型 11，版本 1，REQUIRED；正文见下） | `durability` | 快照切换 |

## 工作记录

类型 2 至 9 由 ADR-0026 定义，均为版本 1 且置 `REQUIRED`。记录写对象在该 transition 之后的状态，不写操作，所以重放不重新执行规则、不调用策略、不读时钟。字节串为 `uint32` 长度加内容；目录 ID 至多 32 字节。

| 类型 | 记录 | 正文 | 长度 |
|---:|---|---|---:|
| 2 | `INSTANCE` | `agent_id` u64、`instance_id` u64、`revision` u32、`lifecycle` u32 | 24 |
| 3 | `GOAL` | `goal_id`、`parent_goal_id`、`owner_instance_id`、`replaced_goal_id`、origin `principal_id`（各 u64）；origin `kind`、`channel`、`binding_revision`、`depth`、`goal_type_version`（各 u32）；`goal_type_id` 字节串；trigger `kind` u32、`delay_us` u64、`period_us` u64、`trigger_source` 字节串；`expiry_after_us` u64、`occurrence_wait_max_us` u64；参数字节串 | 108 + 字节串 |
| 4 | `GOAL_STATE` | `goal_id`、`last_task_id`（各 u64）；`lifecycle`、`revision`、`cancel_revision`、`occurrences_skipped`、`skip_reason`、游标 `taken`、游标 `waiting`、`flags`（各 u32）。`flags` 位 0 = 还有下一次 occurrence，位 1 = 失效尚待发生；其余位须为 0 | 48 |
| 5 | `TASK` | `task_id`、`goal_id`、`owner_instance_id`、`replaced_task_id`（各 u64）；`occurrence`、`occurrence_count`（各 u32） | 40 |
| 6 | `TASK_STATE` | `task_id` u64；`lifecycle`、`revision`、`cancel_revision`、`hold_status`、`hold_reason`、`step_count`（各 u32） | 32 |
| 7 | `TASK_STEPS` | `task_id` u64、`steps_before` u32、`count` u32，然后 `count` 个 `step_key` 字节串：本 transition 追加的步骤，接在已有的 `steps_before` 个之后 | 16 + 字节串 |
| 8 | `TASK_CHECKPOINT` | `task_id` u64、写入方 `version` u32、写入方策略 ID 字节串、私有检查点字节串 | 20 + 字节串 |
| 9 | `COMMAND_MAPPING` | `principal_id`、`command_id`、内容摘要、该 principal 当时的下限（各 u64）；结果的意图 `kind`、对象 `kind`（各 u32）、对象 `id` u64、对象 `revision` u32。应用命令的 `commit_seq` 取自所在批次 | 52 |

- 记录不含单调时间（C-STORE-014）：Goal 写时长，不写到期时刻；Task 的 `due_us` 不写入。恢复时各计时从恢复结束时刻起按完整时长重新计算（ADR-0026 决策 6）。
- 一个批次内的次序：`INSTANCE`，新建 Goal 的 `GOAL`，各 `GOAL_STATE`，新建 Task 的 `TASK`，各 `TASK_STEPS`、`TASK_CHECKPOINT`，各 `TASK_STATE`，最后 `COMMAND_MAPPING`。新建的 Goal 或 Task 须在同一批次带有状态记录，否则为损坏。
- 状态记录引用的对象须已在表中，Goal 与 Task 的 `revision` 不减，`TASK_STEPS` 的 `steps_before` 须等于 Task 已有的步骤数，取代的对象须已终结；Goal 的 owner 须是此前记录启动的实例，终态的 Goal 与 Task 不再改变，带 `hold_status` 停下的 Task 只能结束，实例的 `revision` 不减、停止后不再改变；日志末尾不得有未终结的 Task 留在终态 Goal 之下，也不得有未终结的 Goal 留在终态的父目标之下或属于已停止的实例（结束 Goal 的 transition 同时结束它的后代与 Task，停止实例的 transition 同时结束它的 Goal）。违反者为损坏（C-STORE-009）。新建对象取代的对象记在 `GOAL`、`TASK` 中，重放按记录取代，不重新选择。本次启动的容量或限值放不下的记录使恢复以 `STATE_EXCEEDS_CAPACITY` 受限，任何受限都丢弃已重放的内容。
- 不改变持久状态的 transition 写一个空批次，`commit_seq` 仍然连续。

## 控制记录与读取

- `WRITER_FENCE` 单独成批，新 epoch 必须严格大于前缀现有有效 epoch。其后的完整旧 epoch 帧被忽略，不推进有效 commit、transition 或 boot 基线；其他有效批次仍须连续。
- `SNAPSHOT_COMMIT` 正文为 `slot` u32、`payload_crc` u32、`generation` u64、`covers_commit_seq` u64、`max_transition_id` u64，共 32 字节。它自身是覆盖边界之后的下一批次；维护也消耗统一 transition 身份。迁移激活以 schema 0 外壳写入，之后正常写 schema 1，旧 reader 因 REQUIRED 类型未知而受限。
- 恢复使用 Host 物理 cursor，不能用最大已见 commit_seq 翻页；旧 epoch 的高序号帧可能位于合法新批次之前。cursor 在同一日志实例中严格递增，轮转后从 0 重读。
- 首个完整批次放不下时，Host 以读专用 `READ_BUFFER_TOO_SMALL` 报精确所需长度，映射 `STATE_EXCEEDS_CAPACITY`。真正 I/O 故障、非法大小或 `OK` 越界仍是 `STORAGE_READ_FAILED`。

## 快照

快照采用两个槽。维护先冻结新 transition 并排空 journal，得到边界 `S`，再写非当前槽的内容，回读校验版本、长度与 CRC 后独立写槽尾标记。槽尾标记只证明槽内容完整；日志中的 `SNAPSHOT_COMMIT` 是激活新状态的唯一切换提交，持久化后才轮转旧日志。尚未激活的新槽不参与恢复。

### 槽头与槽尾（schema 1）

头部固定为 72 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYSN` |
| 4 | 2 | `storage_schema` | 1 |
| 6 | 2 | `header_length` | 72 |
| 8 | 8 | `generation` | 从 1 起，每次激活递增 |
| 16 | 8 | `covers_commit_seq` | 快照包含的最后一个提交 `S`；初始空快照为 0 |
| 24 | 8 | `max_transition_id` | 边界 `S` 处最大的有效 transition；不计被 fence 排除的旧写入方 |
| 32 | 8 | `boot_epoch` | 边界 `S` 处最后持久化的启动 epoch；初始空快照为 0 |
| 40 | 4 | `writer_epoch` | 边界 `S` 处有效的写入方代际；初始为 1 |
| 44 | 4 | `flags` | 保留，须为 0 |
| 48 | 8 | `payload_length` | 正文字节数 |
| 56 | 4 | `record_count` | 正文记录数 |
| 60 | 4 | `payload_crc` | 覆盖全部正文，可跨页累计 |
| 64 | 4 | 保留 | 须为 0 |
| 68 | 4 | `header_crc` | 覆盖头部前 68 字节 |

正文紧随头部，其后的独立槽尾固定为 24 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `commit_magic` | ASCII `AYSC` |
| 4 | 8 | `generation` | 与槽头相同 |
| 12 | 4 | `payload_crc` | 与槽头相同 |
| 16 | 4 | `header_crc` | 与槽头相同，绑定全部头部元数据 |
| 20 | 4 | `marker_crc` | 覆盖槽尾前 20 字节 |

读取时先校验头部 CRC，再判断 schema 与 flags；不支持的版本或位进入只读诊断。`generation`、`writer_epoch` 不为 0，`boot_epoch` 不超过 ID 的 32 位 epoch 上限，`max_transition_id` 不超过 2^63−1；初始 `covers_commit_seq=0` 的快照必须为空且 transition 与 boot epoch 均为 0。

### 静态领域记录

正文沿用日志的 12 字节记录信封、类型编号与 `REQUIRED` 位，但**快照上下文的正文语法独立于 transition 日志**。以下均为记录版本 1；不直接写入 C++ 内存布局。

| 类型 | 快照正文 | 长度 |
|---:|---|---:|
| 2 `INSTANCE` | 日志的 `INSTANCE` 正文，随后为该实例最后改变的 `transition_id` u64 与 `commit_seq` u64 | 40 |
| 3 `GOAL` | 日志的 `GOAL` 正文，随后为完整 `GOAL_STATE` 正文，再接原始 `created_transition_id` u64、最后改变的 `commit_seq` u64 | 日志 `GOAL` 长度 + 64 |
| 5 `TASK` | 日志的 `TASK` 正文，随后为原始 `created_transition_id`、最后改变的 `commit_seq`、`steps_seq`、`checkpoint_seq`（各 u64），以及 `steps_before` u32 | 76 |
| 7 `TASK_STEPS` | 日志的步骤正文，每条只含一个 key，`steps_before` 为该 key 的位置、`count=1` | 20 + key 字节数 |
| 8 `TASK_CHECKPOINT` | 与日志正文相同，包含写入方策略 ID、版本及原检查点；没有写入方时不写该记录 | 日志正文长度 |
| 6 `TASK_STATE` | 与日志正文相同，在同一 Task 的全部步骤与检查点之后 | 32 |
| 9 `COMMAND_MAPPING` | 日志正文中的 principal、命令、摘要、最终下限与接受结果，再接原接受批次的 `commit_seq` u64 | 60 |

顺序为全部持久实例（不含 `instance_id=0` 的本次注册项）、按深度从小到大的 Goal、每个 Task 的元数据/逐条步骤/检查点/状态，最后按原接受 `commit_seq` 递增的命令映射。静态导入允许终态父子与已经不存在的 `replaced_goal_id`、`replaced_task_id` 前驱，仍检查 ID 唯一、owner、父子深度、版本、游标、状态与最终跨表一致性。原始创建身份、最新提交、步骤提交及检查点提交均保留，不改写成 `S`，以保持 READY 顺序、终态回收顺序与命令重试结果。

所有对象与命令的提交不超过 `S`，创建 transition 不超过快照基线；本次注册内容的缺失导致的 Agent fault、Goal hold、Task recovery hold 不写入快照。没有注册的 principal 仍占有原有的一行命令窗口及下限；第二个 principal 的持久命令放不下时明确报容量不足，不丢弃原行。命令窗口缩小时按原接受次序让位并提高下限。单调时刻不写入：Goal 只保留时长及是否待发生，Task 不写 `due_us`，最终恢复后重新计时。

导出、回读与恢复按页复用同一存储缓冲，单条记录上限由配置容量计算，整份快照可以大于缓冲；Core 不分配另一份 WorkModel 或完整快照内存。任何损坏、不支持或容量不足均丢弃本次导入的全部状态。

### 激活、轮转与恢复

`SNAPSHOT_COMMIT` 绑定槽、generation、覆盖提交、最大 transition 与正文 CRC；它的批次使用 `commit_seq=S+1`、`transition_id=max_transition_id+1`。schema 0→1 迁移的这个激活批次仍用 schema 0 并置 `REQUIRED`，切换前保留原日志；旧 reader 不能忽略激活记录回到过时状态继续执行。空存储直接建立 schema 1 的栅栏和启动记录，不制造空快照或迁移激活。

恢复先按物理日志次序发现有效 writer fence 与最新激活，再校验并导入它所指的确切槽，最后从 `S+1` 的激活批次起重放尾部。旧激活引用的槽可能已被复用，不逐条读取旧槽；最新激活的槽损坏或不支持时不得回退旧槽。被 fence 排除的旧 epoch 批次不改变激活、提交、transition 或 boot 基线。

激活持久后，轮转保留该激活及之后的物理日志帧；A 保留策略不额外归档已经覆盖的历史。双槽、当前日志与轮转临时输出均有上限；达到维护阈值时冻结新 transition，查询及已映射命令重试仍可读取。轮转中断不撤销已激活的新状态。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-001 | 批次按上表编码：头部、记录区、提交标记；`header_crc` 位于 `header_length − 4` 并覆盖其前的全部头部字节；`header_length` 小于 44 时批次不完整；`header_crc`、`payload_crc` 与 `commit_seq_low` 都校验通过才算完整 | 只检查头部 CRC，记录区短写的批次被当作完整 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_accepted`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-002 | 只有提交标记完整的批次可见；日志末尾不完整的批次被忽略，不部分应用。忽略尾部由 Host 负责（C-HOST-009）：Kernel 把读到的每个批次当作已提交，未通过校验的批次按 C-STORE-009 处理（ADR-0016） | 尾部批次只写了一半记录，恢复时应用了前半 | M1 已实现：Kernel 侧 `BatchCodec.every_short_write_is_corrupt`、`PrefixScan.damaged_batch_stops_the_scan`；Host 丢弃撕裂的尾部 `HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.torn_tail_is_truncated_before_the_next_append`；追加中断电的批次只在完整写入时计入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；重放受限时丢弃已重放的全部内容 `WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`（ADR-0026）；E2 F0、F7、F8 |
| C-STORE-003 | `commit_seq` 从 1（有快照后从快照覆盖的下一个）起连续递增。Host 只交回完整批次，所以读到的缺口或与 Host 分帧不一致的 `commit_seq` 都是已提交区域损坏，按 C-STORE-009 处理（ADR-0016） | `commit_seq` 7 缺失，恢复跳过它继续应用 8 和 9；或把前缀截断在 7 之前，下一次 append 又写出一个 7，日志分叉 | M1 已实现（物理读取、连续前缀与栅栏）：`PrefixScan.commit_seq_must_continue_exactly`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`、`WriterFenceScan.fence_must_advance_the_writer_and_match_its_header`、`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；ADR-0027 |
| C-STORE-004 | 读到 `REQUIRED` 位置位的未知记录类型或版本、未知的 `flags` 位或不支持的 `storage_schema` 时进入 `RESTRICTED`；未置位的未知记录可以跳过 | 新固件写入的授权记录被旧固件跳过，旧固件按更宽的权限运行 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_follow_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_are_skipped`；E4 步骤 6；C-VER-020 |
| C-STORE-005 | 受限栅栏确认旧写入方隔离后，写入 `WRITER_FENCE` 记录（含新 `writer_epoch`）；恢复时忽略栅栏记录之后出现的旧 epoch 批次 | 旧 flush 在重载后落盘，恢复时被当作新批次应用 | M1 已实现（Native 排空、Sim epoch 安装、旧写拒绝和物理扫描）：`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`KernelFence.unavailable_isolation_preserves_diagnostics_without_new_boot`、`SimStorageFaults.fence_rejects_an_owned_late_write_after_its_unknown_completion`、`HostStorage.writer_fence_isolates_append_snapshot_and_trim`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`；动作并存与执行端停止随 `MA-022`；E2 F12 |
| C-STORE-006 | 快照先写内容、回读校验后独立写槽尾标记；双槽与 `generation` 保留旧有效内容，只有绑定该槽的 REQUIRED `SNAPSHOT_COMMIT` 持久化后才激活并轮转。恢复只认最新有效激活绑定的槽，目标损坏或不支持时不回退旧槽 | 新槽内容尚未完整即覆盖唯一旧槽；或最新激活的槽损坏后按旧槽继续执行 | M1 已实现（槽格式、激活/轮转和发布阶段断电）：`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；`SnapshotFaults.power_loss_at_each_publication_request_keeps_the_durable_work`、`SnapshotFaults.corrupt_or_unsupported_active_slot_never_falls_back_to_an_older_slot`、`SnapshotFaults.a_candidate_can_replace_a_superseded_slot_before_an_older_trim_finishes`；C-HOST-011 |
| C-STORE-007 | 恢复从最新激活绑定的快照开始，重放 `commit_seq > covers_commit_seq` 的已提交批次；同一 transition 不重复应用（C-COMMIT-002），对象原始 stamp、READY 顺序、回收顺序、命令结果与下限不被快照覆盖提交改写 | 快照已包含的批次再次应用，或全部对象改用同一覆盖提交，改变任务公平性与回收选择 | M1 已实现（现有工作模型的静态分页快照与尾部重放）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`WorkSnapshot.paged_round_trip_keeps_the_original_stamps_and_checkpoint`、`WorkSnapshot.ready_and_reclamation_choices_match_the_uncompacted_model`、`WorkSnapshot.terminal_parent_and_child_keep_their_reclamation_order`、`WorkSnapshot.a_retained_object_can_name_a_predecessor_already_reclaimed`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`、`WorkSnapshot.smaller_object_and_checkpoint_capacities_report_no_room`、`WorkSnapshot.smaller_goal_parameter_limit_is_capacity_failure`、`WorkSnapshot.object_stamp_beyond_the_snapshot_boundary_is_corrupt`；其余领域随各自工作包；E2 F11 |
| C-STORE-008 | CRC 只用于发现损坏，不作为授权或防篡改依据 | 以 CRC 正确为由信任一条从外部导入的 Grant 记录 | 评审 |
| C-STORE-009 | 已提交区域（不是末尾的不完整批次）出现损坏时进入 `RESTRICTED`，不跳过损坏批次继续应用后续批次 | 中间批次 CRC 错误，恢复跳过它，导致后续撤销记录引用不存在的 Grant | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；M1 已实现（工作记录违反表规则或日志末尾不一致时为 `LOG_CORRUPT`，已重放的内容全部丢弃，ADR-0026 决策 5）：`WorkRecovery.log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt`、`WorkReplay.goal_created_without_its_state_is_corrupt`、`WorkReplay.live_task_under_an_ended_goal_is_inconsistent`、`WorkReplay.live_goal_under_an_ended_parent_or_a_stopped_owner_is_inconsistent`、`GoalRestore.restore_state_refuses_a_change_to_a_terminal_goal_or_an_impossible_cursor`、`TaskRestore.restore_state_refuses_a_change_to_an_ended_or_held_task`、`AgentRestore.record_no_transition_writes_is_corrupt`；E2 F7（已提交区域损坏） |
| C-STORE-010 | 迁移在切换提交前保留旧格式；新 schema 快照经校验和槽尾确认后，以旧 schema 的 REQUIRED 激活批次切换，之后才允许新 schema 写入；中断前未激活的新槽可以重建，激活后不能恢复过时来源执行 | 原地改写旧日志导致新旧都不完整，或旧固件忽略新状态回到旧快照 | M1 已实现（schema 0→1 迁移、切换前保留源和模拟旧 reader 拒绝切换）：`StorageMigration.schema_zero_work_migrates_and_the_old_reader_rejects_the_switch`、`StorageMigration.every_early_request_can_lose_power_without_losing_the_legacy_state`、`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`；E4 步骤 6；C-VER-021 |
| C-STORE-011 | 日志轮转、双槽快照、临时输出、历史大小与写频率均有上限；维护与限频暂时冻结新 transition 并报告背压，动作屏障不能为节省写入而跳过；实际空间不足时新副作用返回 `AYNI_STORAGE_FAILED`，已映射命令仍返回首次结果。保留当前可查询对象、步骤/检查点和既有幂等窗口，不额外归档已覆盖历史 | 为减少磨损跳过屏障，或轮转删除尚未激活快照覆盖的日志，或存储满使已接受命令失去原结果 | M1 部分实现（当前工作模型的轮转、保留、空间准入和写频；动作屏障尚未实现）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`StorageRecovery.retry_wait_is_readable_and_preserves_command_lookup_before_busy`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`；真实空间不足的准入 `StorageAdmission.full_medium_refuses_new_state_but_keeps_the_original_command_result`、`StorageAdmission.an_unmapped_noncommand_also_requires_space_before_applying`；持续限频 `SnapshotFaults.every_mutating_phase_shares_the_interval_and_waits_without_spinning`、`SnapshotFaults.a_slow_strategy_cannot_spend_the_next_storage_interval_before_submit`；动作屏障随 MA-008/MA-018/MA-022；E2 末段；C-COMMIT-015 |
| C-STORE-012 | 日志扇区擦除在没有活动执行器会话时预先完成；会话期间只做页编程，以免擦除挂起停止监督（C-STOP-005） | 会话中日志写满一个扇区，触发擦除，L2 心跳超时误停 | `MA-026` 故障矩阵 |
| C-STORE-013 | 参与 transition 的状态只写入日志；NVS 只保存不参与 transition 的小型配置。必须跨介质原子更新时，以日志中的协调记录实现（C-COMMIT-014） | 预算写在 NVS、Task 写在日志，掉电后只更新了一边 | M1 已实现（工作模型：实例、Goal、Task、步骤、私有检查点与命令映射只写入日志，每个 transition 一个批次，ADR-0026）：`WorkRecovery.power_loss_takes_the_batches_never_appended`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；预算等其余状态随各自的包 |
| C-STORE-014 | 持久记录及快照不包含单调时间值、句柄、指针、资源占用或原始媒体（C-PRIM-001、C-TIME-003）；快照只保存持久领域视图，不把本次目录缺失造成的恢复 fault/hold 永久化 | 快照保存重启前单调时刻，或换回兼容策略后 Task 因上次恢复的临时 hold 仍不能继续 | M1 已实现（现有工作记录和快照：Goal 只写时长，Task 不写 `due_us`，恢复后重新计时）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`WorkSnapshot.bytes_are_independent_of_this_boots_monotonic_clock`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`；评审 |
