# ADR-0016：启动恢复、持久 boot_epoch 与有界 step

- **状态**：已接受（`MA-005` 部分在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §3.2「生命周期」「所有权/异步/线程」、§4.2「持久化」、§4.3；`MA-005`、`MA-010`；E2 F7、F12；ADR-0002、ADR-0006、ADR-0014、ADR-0015

## 背景

M1 第 1 步 A 部分（ADR-0015）给了 Kernel 事件队列与完成槽，但 Kernel 仍只能创建和销毁。`MA-005` 要求 `ayni_start`、`ayni_step`、`ayni_begin_shutdown` 与生命周期表落地；C-ID-009 要求开放任何输出之前持久化递增的 `boot_epoch`；C-HOST-018 要求进入 `STOPPED` 之前全部完成槽都已收到完成。这需要先有最小的持久日志：Host 存储端口的形状、Kernel 怎样读回日志前缀、读到损坏或不认识的内容时怎样处理，以及 step 怎样在不等待 I/O 的前提下推进恢复。完整的重放、快照与写入方栅栏属于 `MA-010`，这里只决定它们依赖的基础。

## 决策

1. **存储端口可选，成对出现。** ABI 0.4 在 `ayni_host_v1` 末尾追加 `storage_append` 与 `storage_read_prefix`。两者都提供或都不提供，只提供一个时 `ayni_create` 返回 `AYNI_INVALID_ARGUMENT`。都不提供的 Host 仍可创建 Kernel 并在 `CONFIGURING` 中使用，但 `ayni_start` 返回 `AYNI_UNSUPPORTED` 并保持 `CONFIGURING`（C-HOST-015），不把“没有日志”当作空日志。Native 与 ESP-IDF Host 目前都不提供存储，持久存储随 `MA-007`、`MA-010` 实现。
2. **Host 按批次存取，Kernel 仍校验每个批次。** `storage_append` 交给 Host 一个已编码的批次及其 `commit_seq`；Host 自行分帧，保证批次要么完整可读，要么不存在，写入中途掉电也如此（C-HOST-009、C-HOST-010）。`storage_read_prefix(from_commit_seq, buffer)` 按日志顺序复制 `commit_seq ≥ from_commit_seq` 的完整批次，能放下几个就放几个；完成值为写入的字节数，0 表示没有更多批次；下一个批次大于缓冲时以 `AYNI_WRITE_FAILED` 完成。Kernel 不信任 Host 的分帧，仍按 `storage/records.md` 检查每个批次的头部 CRC、记录区 CRC 与提交标记。
3. **一块存储缓冲。** `ayni_config_v1` 追加 `storage_buffer_bytes`（256–65536，默认 1024），从调用方内存块中划出（ADR-0014）。恢复期间同一时刻只有一个存储请求在途，所以读取与写入共用这块缓冲；它也是单个批次大小的上限。
4. **恢复先读完整个前缀，再持久化新 epoch。** `ayni_start` 只把状态改为 `RECOVERING`，不调用 Host；此后每次 `ayni_step` 最多发出一个存储请求，结果在之后的 step 中应用。
   - 从 `commit_seq` 1 起分块读取，直到读到 0 字节。每个批次的 `commit_seq` 必须恰好是下一个期望值。
   - Host 只返回完整批次，所以 Kernel 读到的每个批次都属于已提交区域：校验失败或 `commit_seq` 不连续都是损坏（C-STORE-009），进入 `RESTRICTED`，不跳过，也不截断。C-STORE-003 据此修订：缺口不再表示前缀在缺口前结束。
   - 不认识的 `storage_schema`、批次 `flags` 位、记录 `flags` 位，或置 `REQUIRED` 位的未知记录类型或版本，进入 `RESTRICTED`（C-STORE-004）；未置位的未知记录跳过。
   - `BOOT_EPOCH` 记录类型为 1、版本 1，正文是 8 字节小端 epoch，并置 `REQUIRED` 位，旧固件无法跳过它。日志中的 epoch 必须严格递增且不为 0，否则视为损坏。
   - 读完后写一个只含 `BOOT_EPOCH` 的批次：epoch 为日志中的最大值加 1，`commit_seq` 为下一个值，`transition_id` 为已见最大值加 1，`writer_epoch` 为已见最大值（空日志为 1）。最大 epoch 已是 `UINT64_MAX` 时进入 `RESTRICTED`，不回绕。
   - append 以 `AYNI_WRITE_OK` 完成后才进入 `RUNNING`，`ayni_get_kernel_info` 从此报告该 epoch（C-ID-009）。`FAILED`、`UNKNOWN` 与未定义的 outcome 都进入 `RESTRICTED`。`UNKNOWN` 时批次可能已经落盘，下一次启动读到它就会跳过这个 epoch，所以同一 epoch 不会被两次启动使用。
   - 恢复期间的请求以 `boot_epoch` 0 命名（C-ID-006），因为本次启动的 epoch 尚未持久化。上一次启动的完成不会混入：Kernel 只有在全部完成槽都已完成之后才能销毁（C-HOST-018）。
5. **`RESTRICTED` 带原因。** 新增 `ayni_restriction_t`：`STORAGE_REJECTED`、`STORAGE_READ_FAILED`、`STORAGE_WRITE_FAILED`、`STORAGE_WRITE_UNKNOWN`、`LOG_CORRUPT`、`LOG_UNSUPPORTED`、`BOOT_EPOCH_EXHAUSTED`，经 `ayni_get_kernel_info` 读取，只在 `RESTRICTED` 状态下不为 `NONE`。M1 中进入 `RESTRICTED` 之后只能停机；自动重试与受限栅栏随 `MA-010`。
6. **step 有界且不等待。** `ayni_step(max_items, max_time_us)` 逐个应用已投递的完成，每个算一项，每项之后读一次 Host 时钟，超过 `max_time_us` 即返回；随后在 `RECOVERING` 中根据结果改变状态，预算仍有余时发出至多一个存储请求，也算一项。报告的 `more_ready` 表示仍有已投递的完成，或恢复已有下一个请求待发；`next_deadline_us` 暂为 `UINT64_MAX`，定时工作随 `MA-004` 及之后的工作包加入。
7. **停机在 step 中完成。** `ayni_begin_shutdown` 从 `RECOVERING`、`RUNNING`、`RESTRICTED` 进入 `DRAINING`，在 `DRAINING`、`STOPPED` 中幂等返回 `AYNI_OK`，在 `CONFIGURING` 中返回 `AYNI_INVALID_STATE`（此时直接 `ayni_destroy` 即可）。`DRAINING` 中不再发出新请求，已在途的请求照常完成并应用；在途槽数为 0 时，下一次 step 进入 `STOPPED`。

## 后果

- 任何 Host 都可以暂不实现存储；需要恢复的部署必须实现两个端口，并由 Host 负责分帧与掉电原子性，Host 契约测试套件（C-HOST-017）须覆盖这两点。
- Kernel 不区分“尾部撕裂”与“中间损坏”，所以 Host 必须丢弃自己没写完的尾部批次。Host 若把撕裂的尾部交给 Kernel，结果是 `RESTRICTED`：偏保守，但不会悄悄跳过数据。
- 每次启动写一个批次，读取量随启动次数线性增加；日志轮转与快照（`MA-010`）把读取限制在快照之后。M1 读取整个日志，耗时与日志长度成正比，但分摊在多次有界 step 中。
- 缓冲大小限制最大批次。超过缓冲的批次使恢复以 `STORAGE_READ_FAILED` 停下，所以缩小 `storage_buffer_bytes` 的固件升级需要先迁移日志。
- ABI 升至 0.4：`ayni_config_v1` 与 `ayni_host_v1` 追加字段；新增 `ayni_bytes_v1`、`ayni_storage_batch_v1`、`ayni_kernel_info_v1`、`ayni_step_report_v1` 与四个函数，这些声明从 `include/ayni/draft/` 移入 `ayni.h`（C-VER-007）。
- Core 新增 `foundation`（CRC-32、小端读写）与 `durability` 两个模块。日志格式的规则归 `durability`：`batch/batch_codec` 编解码并校验单个批次，`batch/prefix_scan` 检查 `commit_seq` 连续性与记录 flags，并把每条记录交给其类型的属主判断；`kernel/boot` 只解释 `BOOT_EPOCH` 并驱动存储请求。`kernel/boot` 依赖 `kernel/completion`，已在 `tools/architecture.json` 声明。`MA-010` 的重放沿用同一扫描，让各属主模块处理自己的记录。

## 备选方案

- **Host 按字节偏移返回原始日志，由 Kernel 分帧并判断尾部**：Kernel 要区分撕裂尾部与中间损坏，还要处理跨越缓冲边界的批次；而扇区、文件长度这类判断写入是否完整所需的介质知识在 Host 一侧。否决。
- **每次读取只返回一个批次**：接口更简单，但长日志需要成倍的往返与 step。否决。
- **`ayni_start` 同步读取日志**：违背 C-LIMIT-011，API 不等待存储。否决。
- **没有存储端口时 `ayni_start` 直接进入 `RUNNING`**：等于桩函数报告成功（C-HOST-015），重启后 `boot_epoch` 不递增，违背 C-ID-009。否决。
- **`commit_seq` 有缺口时把前缀截断在缺口前（原 C-STORE-003）**：随后的 append 会以已被占用的 `commit_seq` 写在缺口之后的批次后面，日志分叉。否决，改为按损坏处理。
- **epoch 只取最大值，不要求递增**：容忍了无法解释的日志；递增检查几乎没有成本，还能发现 Host 返回旧批次或打乱顺序。否决。

## 验证

- `tests/unit/batch_codec_test.cpp`（`Crc32.*`、`BatchCodec.*`、`BatchWriter.*`）：CRC-32 校验值；往返；任意一个字节损坏、任意长度的短写都识别为损坏；更长的头部被接受；其他 `storage_schema` 或未知 `flags` 识别为不支持；提交标记绑定 `commit_seq`；记录数与记录区不符；记录越界；写入时为提交标记预留空间。
- `tests/unit/prefix_scan_test.cpp`（`PrefixScan.*`）：跨多次读取保持 `commit_seq` 连续并记录最大 `transition_id` 与 `writer_epoch`；缺口、重复与起点错误是损坏；损坏的批次之后不再交出记录；未知记录按 `REQUIRED` 位处理，未知记录 flags 不支持，属主判定的错误正文是损坏。
- `tests/unit/kernel_lifecycle_test.cpp`（`KernelLifecycle.*`）：首次启动写入 epoch 1，且只在 append 完成后进入 `RUNNING`；每次重启递增；小缓冲分块读取长日志；未知可选记录被跳过；损坏、缺口、日志不从 1 开始、Host 与批次的 `commit_seq` 不一致、epoch 不递增或为 0、正文长度错误、未知必需记录或版本、未知记录 flags、其他 schema 与 epoch 耗尽各自进入 `RESTRICTED` 且不写日志；读与写的拒绝、失败、未知各自带对应原因；超过缓冲的批次与超过缓冲的字节数；恢复请求使用 `boot_epoch` 0；恢复中停机等待在途请求、`DRAINING` 中不再发请求、`STOPPED` 之前 `ayni_destroy` 返回 `AYNI_INVALID_STATE`；六个状态下六个函数的返回值表；参数检查；`max_items` 与时间预算。
- `tests/unit/kernel_api_test.cpp`：只提供一个存储端口时创建失败；没有存储端口的 Host 表被接受；`storage_buffer_bytes` 的范围与默认值。`tests/contract/abi_layout.c` 固定新结构体的布局，`tests/contract/c_abi_smoke.c` 从 C99 调用新函数。
- 以上测试在本地以 GCC Debug、Release、ASan/UBSan、TSan 与 Clang Debug 运行通过，CI 的 `native` 任务运行同样的组合。Native 与 ESP-IDF Host 尚无存储端口，持久化只在模拟 Host 上验证；掉电与短写注入随 `MA-010`。
