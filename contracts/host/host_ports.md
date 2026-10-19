# Host 端口

- **关联**：计划 §2.1、§3.2「所有权/异步/线程」、§4.1、§4.2、§4.3；ADR-0002、ADR-0006、ADR-0014；`MA-003`、`MA-005`、`MA-007`、`MA-010`；E1 步骤 10，E2 F2、F7、F12
- **属主**：`kernel`（端口的调用方与事件入口）；各 Host 实现（`hosts/native`、`hosts/esp_idf`）
- **头文件**：已实现部分 `include/ayni/ayni.h`（`ayni_host_v1`：时钟）；计划部分 `include/ayni/draft/host_v1.h`

Host 为 Kernel 维护自身不变量提供平台服务（ADR-0006）：时钟、持久存储、唤醒与日志出口。网络、模型、媒体与设备 I/O 不是 Host 端口，而是经 Binding 调用的 Provider（`provider.md`）。

## 端口一览

| 端口 | 方向 | 调用线程 | 是否阻塞 | 完成方式 | 状态 |
|---|---|---|---|---|---|
| `monotonic_now_us` | Kernel → Host | Kernel 所有者线程 | 否 | 同步返回 | M0 已实现 |
| `wake` | Host 内部 / 任意线程 → Kernel 所有者线程 | 任意线程（ISR 经桥接） | 否 | 触发所有者线程调用 `ayni_step` | 草案 |
| `storage_append` | Kernel → Host | Kernel 所有者线程 | 否 | 完成：`OK` / `FAILED` / `UNKNOWN` | 草案 |
| `storage_read_prefix` | Kernel → Host | Kernel 所有者线程（`RECOVERING`） | 否 | 分块完成：读入字节数 | 草案 |
| `storage_fence_writers` | Kernel → Host | Kernel 所有者线程（受限栅栏） | 否 | 完成：`DRAINED` / `EPOCH_INSTALLED` / `UNAVAILABLE` | 草案 |
| `storage_write_snapshot`、`storage_read_snapshot` | Kernel → Host | Kernel 所有者线程 | 否 | 完成：`OK` / `FAILED` / `UNKNOWN`；读取另报字节数 | 草案 |
| `storage_free_bytes` | Kernel → Host | Kernel 所有者线程 | 否 | 同步返回剩余可用字节 | 草案 |
| `log` | Kernel → Host | Kernel 所有者线程 | 否（可丢弃并计数） | 无 | 草案 |
| `ayni_post_event` | Host → Kernel | Host 声明的事件线程，有界 | 否 | 返回 `OK` / `BUSY` | 草案（`include/ayni/draft/kernel_api.h`） |
| `ayni_completion_post` | Host → Kernel | 任意非 ISR 线程 | 否 | 写入预留的完成槽 | 草案 |

端口名与 `include/ayni/draft/host_v1.h` 的成员名一致。

## 完成槽

控制类完成（存储确认、调用终态、取消结果、停止确认）在 `submit`/`append` **受理时**预留一个完成槽：

1. Kernel 先从预留池取一个槽，为这次尝试分配 `(boot_epoch, request_id)`，再连同二者调用 Host；没有空槽时 Kernel 不调用 Host，调用方得到 `AYNI_BUSY`。
2. Host 完成时以同一 `(boot_epoch, request_id)` 把结果写入该槽并 `wake`；写槽不会因普通事件队列满而失败。
3. Kernel 在所有者线程上应用结果并释放槽，之后该槽可以分给新的尝试。结果所带的尝试身份与槽当前的尝试不一致（迟到或重复的完成）时忽略并计数，所以槽被重用后旧结果也不会被当作新尝试的结果。
4. Host 对每个已受理的请求恰好完成一次，结果未知时也要以 `UNKNOWN` 完成；完成之前 Host 可以持有槽指针与请求引用的 Kernel 缓冲，完成之后两者都不再访问。全部预留槽都已完成，Kernel 才能进入 `STOPPED`。

在途请求数有上限，所以预留池有界；可靠性逻辑留在可移植 Core，不在各 Host 重复实现确认与重投协议。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-HOST-001 | `ayni_create` 复制 Host 函数表；`ctx` 与函数指针指向的对象由调用方保持有效，直到 `ayni_destroy` 返回 `AYNI_OK` | 调用方在栈上构造 Host 表，`ayni_create` 返回后表失效，Kernel 读到垃圾指针 | M0 已实现：`KernelApi.create_copies_host_table_before_return` |
| C-HOST-002 | 除声明为任意线程可调用的端口外，Host 函数只在 Kernel 所有者线程上被调用；Host 函数不阻塞，不在函数内回调 Kernel API | 存储 Host 在 `append` 内同步等 Flash 写完，`step` 被卡住数十毫秒 | `MA-003`；`MA-007`；评审 |
| C-HOST-003 | 异步结果以事件回送，经 `ayni_post_event` 或预留完成槽进入 Kernel，并携带 `(boot_epoch, request_id)` | Host 在网络线程里直接修改 Kernel 内部的 Task 状态 | `MA-003`；E2 F7 |
| C-HOST-004 | `submit` 类端口的同步返回区分三种结果：确定拒绝且未保留任何工作；已受理；受理结果未知。后两者都视为在途，Host 内部排队也算在途 | Host 把请求放进内部队列后返回“拒绝”，撤销时该请求未被处理却仍被执行 | E2 F10；C-OUT-003 |
| C-HOST-005 | 取消是请求：Host 或 Provider 以事件报告取消成功、无法取消或结果未知；取消请求返回不代表已取消 | 取消函数返回 OK 后 Kernel 立即释放了执行器资源 | E1 步骤 4；C-CALL-009 |
| C-HOST-006 | 控制类完成在受理时预留完成槽，槽不足时返回 `AYNI_BUSY`；存储确认与终态不因队列满而丢弃；每个完成携带它回答的 `(boot_epoch, request_id)`，与槽当前尝试不一致的完成和重复完成都被忽略，槽重用后也只应用一次；遥测与流事件按声明合并或丢弃并报告 gap | 普通队列满时存储确认被丢弃，Kernel 永远等不到屏障确认；或槽重用后，上一次 append 的重复 `OK` 被当作这一次 append 的持久确认 | E1 步骤 10；`MA-003` |
| C-HOST-007 | `wake` 可以从任意线程调用、可重入且幂等；多次唤醒可以合并为一次 `step` 调度 | 两个线程同时唤醒导致所有者线程运行两份 `step` | `MA-003`；`MA-007` |
| C-HOST-008 | ISR 不调用任何 Kernel API；ISR 经 Host 提供的 ISR 安全桥接写入事件或最新值寄存器，再由 Host 唤醒所有者线程 | 按键中断服务程序直接调用 `ayni_post_event` | 评审；`MA-007` |
| C-HOST-009 | 存储端口提供 `storage_append`、`storage_read_prefix`、`storage_fence_writers`、`storage_write_snapshot`、`storage_read_snapshot` 与 `storage_free_bytes`；`append` 与快照写入以 `OK`、`FAILED`、`UNKNOWN` 之一完成，`read_prefix` 只返回连续且提交标记有效的批次 | `read_prefix` 返回了提交标记缺失的尾部批次 | `MA-010`；E2 F7、F8 |
| C-HOST-010 | 存储 Host 只有在批次已按其声明的原子性落到介质、能经受掉电时才报告 `OK`；数据仅进入写缓冲或 Host 队列时不得报告 `OK` | 文件 Host 在 `write()` 返回后、`fsync()` 之前报告 OK | `MA-010` 掉电注入；C-COMMIT-001 |
| C-HOST-011 | 存储 Host 声明自己是否提供断电原子的替换与 flush；不提供时，Core 使用双槽与提交标记（`storage/records.md`），并用故障注入验证 | ESP32 Host 假定 FAT 的 rename 断电原子，快照切换中掉电后两份都损坏 | `MA-010`；E2 F7 |
| C-HOST-012 | `fence_writers` 以 `DRAINED`（旧 append/flush 全部终止）、`EPOCH_INSTALLED`（新 writer epoch 已原子安装，旧 epoch 写入被拒绝）或 `UNAVAILABLE` 完成；`UNAVAILABLE` 时 Kernel 保持 `RESTRICTED` | Host 在旧 flush 仍在进行时报告 `DRAINED`，重载后旧写落盘使日志分叉 | E2 F12；C-COMMIT-010、C-COMMIT-011 |
| C-HOST-013 | 存储端口报告剩余可用空间；Kernel 在空间不足以保存动作屏障时拒绝新副作用 | 日志分区只剩几个字节，屏障写入失败后才发现，期间副作用已排队 | E2 末段；C-COMMIT-015 |
| C-HOST-014 | 日志出口只接收 Kernel 已脱敏的 trace 记录；Host 不得把凭据、私有记忆正文或媒体写入日志；日志出口满时丢弃并计数，不阻塞 Kernel | 串口日志打印出 Wi-Fi 密码或网关令牌 | 评审；第 10.6 节「安全接入」 |
| C-HOST-015 | Host 不提供的功能显式返回 `AYNI_UNSUPPORTED`；不得用桩函数返回成功 | 没有持久存储的 Host 在 `append` 中直接报告 OK | `MA-005`；`MA-007` |
| C-HOST-016 | Host 函数表遵守 C-VER-002/C-VER-003：以 `struct_size` 开头，只在末尾追加；必需端口缺失时 `ayni_create` 返回 `AYNI_INVALID_ARGUMENT` | 缺少时钟的 Host 表被接受，Kernel 第一次计算期限时空指针崩溃 | M0 已实现：`KernelApi.create_rejects_missing_clock` |
| C-HOST-017 | 新 Host 必须通过共享的 Host 契约测试套件后才能标记为已验证；平台差异不得改变本文的语义 | ESP32 Host 的 `append` 在写缓冲时报告 OK，Native Host 在 fsync 后报告 OK，同一测试在两边结果不同 | M1 `tests/contract/` Host 套件；M2 ESP32 复核 |
| C-HOST-018 | 已受理（`ACCEPTED` 或 `ACCEPT_UNKNOWN`）的异步请求，Host 或 Provider 恰好完成一次，结果未知时以 `UNKNOWN`（Provider 为 `ABANDONED`）完成；全部预留槽完成后 Kernel 才进入 `STOPPED`。组合根在调用 `ayni_destroy` 之前停止所有可能调用 `ayni_post_event`、`ayni_completion_post` 或 `ayni_provider_post` 的线程，这些函数不与 `ayni_create`、`ayni_destroy` 并发 | 停机把一次结果未知的 append 记为未知后进入 `STOPPED`，内存被释放，存储线程随后向已释放的槽写入完成 | `MA-003`；`MA-005` 停机用例；`MA-007` |
| C-HOST-019 | 异步端口引用的 Kernel 缓冲（批次与快照的字节、读取缓冲）在该尝试完成之前由 Kernel 保持有效且不修改，Host 可以从 I/O 线程读写；完成之后 Host 不再访问 | Kernel 在 append 受理后立即复用批次缓冲编码下一个 transition，Host 的 I/O 线程写出新旧混杂的批次 | `MA-010`；Host 契约测试套件 |
