# 持久记录格式

- **关联**：计划 §2.8、§4.2「持久化」、§4.3、§4.4「升级」；ADR-0002、ADR-0016；`MA-010`；E2 F0–F8、F11、F12，E4 步骤 6
- **属主**：`durability`（批次编码、持久前缀、快照、重放、栅栏记录、迁移机制）；领域记录的内容由各属主模块定义，`durability` 不理解其语义
- **状态**：本文是 M1 实现的起点。字节布局为草案，M1 以故障注入（短写、掉电、CRC 错误、旧写延迟）验证后定稿，定稿前 `storage_schema` 为 0（experimental）

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
| `WRITER_FENCE` | `durability` | 单独批次（C-STORE-005） |
| `COMMAND_MAPPING` | `kernel` | B1 |
| `GOAL`、`TASK` | `work/goal`、`work/task` | B1、B2 |
| `OCCURRENCE_CURSOR` | `work/goal` | B2 |
| `BUDGET_RESERVATION`、`BUDGET_SETTLEMENT` | `budget` | B2、B4 |
| `STEP_CHECKPOINT`、`CALL_IDENTITY` | `work/task`、`work/call` | B3 |
| `ACTION_STATE` | `work/action` | 准备、屏障与终态各自的批次；终态在 B4 |
| `CALL_RESULT`、`TASK_PROGRESS` | `work/call`、`work/task` | B4 |
| `MEMORY_RECORD`、`MEMORY_TOMBSTONE`、`LINEAGE` | `knowledge` | B4、B5 |
| `GRANT`、`GRANT_REVOCATION` | `authority/grant` | 授权变更批次、B5 |
| `INVALIDATION_CURSOR` | `knowledge/lineage` | B5 |
| `SNAPSHOT_COMMIT` | `durability` | 快照切换 |

## 快照

快照写入新内容、校验版本/长度/CRC 后再提交切换。存储 Host 不提供断电原子的替换与 flush 时（C-HOST-011），使用两个槽：

| 字段 | 说明 |
|---|---|
| `magic`、`storage_schema` | ASCII `AYSN` 与格式版本 |
| `generation` | 每次切换递增 |
| `covers_commit_seq` | 快照包含的最后一个 `commit_seq` |
| `payload_length`、`payload_crc` | 快照内容 |
| `commit_marker` | 内容写完并校验后才写入 |

恢复选 `commit_marker` 有效且 `generation` 最大的槽，再重放 `commit_seq > covers_commit_seq` 的已提交批次。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-001 | 批次按上表编码：头部、记录区、提交标记；`header_crc` 位于 `header_length − 4` 并覆盖其前的全部头部字节；`header_length` 小于 44 时批次不完整；`header_crc`、`payload_crc` 与 `commit_seq_low` 都校验通过才算完整 | 只检查头部 CRC，记录区短写的批次被当作完整 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_accepted`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-002 | 只有提交标记完整的批次可见；日志末尾不完整的批次被忽略，不部分应用 | 尾部批次只写了一半记录，恢复时应用了前半 | E2 F0、F7、F8 |
| C-STORE-003 | `commit_seq` 从 1（有快照后从快照覆盖的下一个）起连续递增。Host 只交回完整批次，所以读到的缺口或与 Host 分帧不一致的 `commit_seq` 都是已提交区域损坏，按 C-STORE-009 处理（ADR-0016） | `commit_seq` 7 缺失，恢复跳过它继续应用 8 和 9；或把前缀截断在 7 之前，下一次 append 又写出一个 7，日志分叉 | M1 已实现：`PrefixScan.commit_seq_must_continue_exactly`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；快照之后的起点随 `MA-010` |
| C-STORE-004 | 读到 `REQUIRED` 位置位的未知记录类型或版本、未知的 `flags` 位或不支持的 `storage_schema` 时进入 `RESTRICTED`；未置位的未知记录可以跳过 | 新固件写入的授权记录被旧固件跳过，旧固件按更宽的权限运行 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_follow_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_are_skipped`；E4 步骤 6；C-VER-020 |
| C-STORE-005 | 受限栅栏确认旧写入方隔离后，写入 `WRITER_FENCE` 记录（含新 `writer_epoch`）；恢复时忽略栅栏记录之后出现的旧 epoch 批次 | 旧 flush 在重载后落盘，恢复时被当作新批次应用 | E2 F12；C-COMMIT-010 |
| C-STORE-006 | 快照先写内容、校验后再写提交标记；没有断电原子替换时使用双槽与 `generation`，恢复选提交标记有效且 `generation` 最大的槽 | 覆盖写唯一的快照文件时掉电，旧快照和新快照都不可用 | `MA-010` 掉电注入；C-HOST-011 |
| C-STORE-007 | 恢复从最近有效快照开始，重放 `commit_seq > covers_commit_seq` 的已提交批次；同一 transition 重放不重复应用（C-COMMIT-002） | 快照已包含的批次被再次重放，预算结算了两次 | E2 F11；`MA-010` |
| C-STORE-008 | CRC 只用于发现损坏，不作为授权或防篡改依据 | 以 CRC 正确为由信任一条从外部导入的 Grant 记录 | 评审 |
| C-STORE-009 | 已提交区域（不是末尾的不完整批次）出现损坏时进入 `RESTRICTED`，不跳过损坏批次继续应用后续批次 | 中间批次 CRC 错误，恢复跳过它，导致后续撤销记录引用不存在的 Grant | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`；E2 F7（已提交区域损坏） |
| C-STORE-010 | 迁移在切换提交前保留旧格式；迁移结果以新 `storage_schema` 写入并经校验后才切换 | 原地改写日志段为新格式时掉电，两种格式都无法读取 | E4 步骤 6；C-VER-021 |
| C-STORE-011 | 日志轮转、快照与历史大小、写频率都有上限；动作屏障不能为节省写入而跳过；空间不足时新副作用返回 `AYNI_STORAGE_FAILED` | 为减少 Flash 磨损，把屏障与终态合并为一次写入 | E2 末段；C-COMMIT-015 |
| C-STORE-012 | 日志扇区擦除在没有活动执行器会话时预先完成；会话期间只做页编程，以免擦除挂起停止监督（C-STOP-005） | 会话中日志写满一个扇区，触发擦除，L2 心跳超时误停 | `MA-026` 故障矩阵 |
| C-STORE-013 | 参与 transition 的状态只写入日志；NVS 只保存不参与 transition 的小型配置。必须跨介质原子更新时，以日志中的协调记录实现（C-COMMIT-014） | 预算写在 NVS、Task 写在日志，掉电后只更新了一边 | `MA-010` 故障注入 |
| C-STORE-014 | 持久记录不包含单调时间值、句柄、指针、资源占用或原始媒体（C-PRIM-001、C-TIME-003） | 观察记录保存了重启前的单调时间戳作为有效期 | 评审；`MA-010` 记录编码测试 |
