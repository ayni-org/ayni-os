# Host 端口

- **关联**：计划 §2.1、§3.2「所有权/异步/线程」、§4.1、§4.2、§4.3；ADR-0002、ADR-0006、ADR-0014、ADR-0015、ADR-0016、ADR-0019、ADR-0020、ADR-0021、ADR-0026；`MA-003`、`MA-005`、`MA-007`、`MA-010`；E1 步骤 10，E2 F2、F7、F12
- **属主**：`kernel`（端口的调用方、事件入口与完成槽）；各 Host 实现（`hosts/native`、`hosts/esp_idf`、`hosts/sim`）
- **头文件**：已实现部分 `include/ayni/ayni.h`（`ayni_host_v1`：时钟、唤醒、入口临界区、日志追加与前缀读取；`ayni_completion_post`；`ayni_post_event`）；计划部分 `include/ayni/draft/host_v1.h`

Host 为 Kernel 维护自身不变量提供平台服务（ADR-0006）：时钟、持久存储、唤醒与日志出口。网络、模型、媒体与设备 I/O 不是 Host 端口，而是经 Binding 调用的 Provider（`provider.md`）。

## 端口一览

| 端口 | 方向 | 调用线程 | 是否阻塞 | 完成方式 | 状态 |
|---|---|---|---|---|---|
| `monotonic_now_us` | Kernel → Host | Kernel 所有者线程 | 否 | 同步返回 | M0 已实现 |
| `wake` | Kernel 或 Host 线程 → Kernel 所有者线程 | 任意非 ISR 线程（ISR 经桥接） | 否 | 多次唤醒合并为所有者线程的一次调度 | M1 已实现 |
| `ingress_lock`、`ingress_unlock` | Kernel → Host | 任意非 ISR 线程 | 只等待持锁者的一步有界操作（C-HOST-020） | 同步返回 | M1 已实现 |
| `storage_append` | Kernel → Host | Kernel 所有者线程 | 否 | 完成：`OK` / `FAILED` / `UNKNOWN` | M1 已实现（Kernel 侧与模拟 Host；可选，与 `storage_read_prefix` 成对） |
| `storage_read_prefix` | Kernel → Host | Kernel 所有者线程（`RECOVERING`） | 否 | 分块完成：读入的完整批次字节数，0 表示没有更多 | M1 已实现（Kernel 侧与模拟 Host） |
| `storage_fence_writers` | Kernel → Host | Kernel 所有者线程（受限栅栏） | 否 | 完成：`DRAINED` / `EPOCH_INSTALLED` / `UNAVAILABLE` | 草案 |
| `storage_write_snapshot`、`storage_read_snapshot` | Kernel → Host | Kernel 所有者线程 | 否 | 完成：`OK` / `FAILED` / `UNKNOWN`；读取另报字节数 | 草案 |
| `storage_free_bytes` | Kernel → Host | Kernel 所有者线程 | 否 | 同步返回剩余可用字节 | 草案 |
| `log` | Kernel → Host | Kernel 所有者线程 | 否（可丢弃并计数） | 无 | 草案 |
| `ayni_post_event` | Host → Kernel | 任意非 ISR 线程 | 只等待入口锁 | 返回 `OK`（已入队，解锁后 `wake`）/ `BUSY`（队列满）/ `INVALID_STATE`（`CONFIGURING`、`STOPPED`） | M1 已实现（本地输入，ADR-0021；其他事件种类为草案） |
| `ayni_completion_post` | Host → Kernel | 任意非 ISR 线程 | 只等待入口锁 | 写入预留的完成槽，然后 `wake` | M1 已实现 |

端口名与 `ayni_host_v1`（已实现部分）和 `include/ayni/draft/host_v1.h`（计划部分）的成员名一致。

## 完成槽

控制类完成（存储确认、调用终态、取消结果、停止确认）在 `submit`/`append` **受理时**预留一个完成槽：

1. Kernel 先从预留池取一个槽，为这次尝试分配 `(boot_epoch, request_id)`，再连同二者调用 Host；没有空槽时 Kernel 不调用 Host，调用方得到 `AYNI_BUSY`。Host 同步返回 `REJECTED_NOT_RETAINED` 时槽立即释放（C-HOST-004）。
2. Host 完成时以同一 `(boot_epoch, request_id)` 把结果写入该槽并 `wake`；写槽不会因普通事件队列满而失败。
3. Kernel 在所有者线程上应用结果并释放槽，之后该槽可以分给新的尝试。结果所带的尝试身份与槽当前的尝试不一致（迟到或重复的完成）时忽略并计数，所以槽被重用后旧结果也不会被当作新尝试的结果。
4. Host 对每个已受理的请求恰好完成一次，结果未知时也要以 `UNKNOWN` 完成；完成之前 Host 可以持有槽指针与请求引用的 Kernel 缓冲，完成之后两者都不再访问。全部预留槽都已完成，Kernel 才能进入 `STOPPED`。

在途请求数有上限，所以预留池有界；可靠性逻辑留在可移植 Core，不在各 Host 重复实现确认与重投协议。槽的状态变更都在入口临界区内进行（ADR-0015）。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-HOST-001 | `ayni_create` 复制 Host 函数表；`ctx` 与函数指针指向的对象由调用方保持有效，直到 `ayni_destroy` 返回 `AYNI_OK` | 调用方在栈上构造 Host 表，`ayni_create` 返回后表失效，Kernel 读到垃圾指针 | M0 已实现：`KernelApi.create_copies_host_table_before_return` |
| C-HOST-002 | 除声明为任意线程可调用的端口（`wake`、`ingress_lock`、`ingress_unlock`）外，Host 函数只在 Kernel 所有者线程上被调用；Host 函数不阻塞（入口锁只等待 C-HOST-020 规定的有界操作），不在函数内回调 Kernel API | 存储 Host 在 `append` 内同步等 Flash 写完，`step` 被卡住数十毫秒 | `MA-003`；`MA-007`；评审 |
| C-HOST-003 | 异步结果以事件回送，经 `ayni_post_event` 或预留完成槽进入 Kernel，并携带 `(boot_epoch, request_id)` | Host 在网络线程里直接修改 Kernel 内部的 Task 状态 | `MA-003`；E2 F7 |
| C-HOST-004 | `submit` 类端口的同步返回区分三种结果：确定拒绝且未保留任何工作；已受理；受理结果未知。后两者都视为在途，Host 内部排队也算在途；Kernel 把未定义的返回值当作受理结果未知并计数 | Host 把请求放进内部队列后返回“拒绝”，撤销时该请求未被处理却仍被执行 | M1 已实现（Kernel 侧）：`CompletionPoolTest.rejected_request_frees_the_slot_and_runs_no_handler`、`CompletionPoolTest.accept_unknown_waits_for_the_post`、`CompletionPoolTest.undefined_submit_result_counts_as_in_flight`、`CompletionPoolTest.post_before_a_rejection_is_still_applied`；E2 F10；C-OUT-003 |
| C-HOST-005 | 取消是请求：Host 或 Provider 以事件报告取消成功、无法取消或结果未知；取消请求返回不代表已取消 | 取消函数返回 OK 后 Kernel 立即释放了执行器资源 | E1 步骤 4；C-CALL-009 |
| C-HOST-006 | 控制类完成在受理时预留完成槽，槽不足时返回 `AYNI_BUSY`；存储确认与终态不因队列满而丢弃；每个完成携带它回答的 `(boot_epoch, request_id)`，与槽当前尝试不一致的完成和重复完成都被忽略，槽重用后也只应用一次；遥测与流事件按声明合并或丢弃并报告 gap | 普通队列满时存储确认被丢弃，Kernel 永远等不到屏障确认；或槽重用后，上一次 append 的重复 `OK` 被当作这一次 append 的持久确认 | M1 已实现（完成槽部分）：`CompletionPoolTest.exhausted_slots_return_busy_without_calling_the_port`、`CompletionPoolTest.result_is_applied_once`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`、`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionPoolTest.concurrent_posts_apply_each_result_exactly_once`、`KernelCompletions.posts_succeed_while_the_event_queue_is_full`；遥测合并见 C-LIMIT-008，订阅的 gap 报告随订阅实现；E1 步骤 10；`MA-003` |
| C-HOST-007 | `wake` 可以从任意非 ISR 线程调用（ISR 经 C-HOST-008 的桥接），可重入且幂等；多次唤醒可以合并为一次 `step` 调度 | 两个线程同时唤醒导致所有者线程运行两份 `step` | M1 已实现：`NativeHost.wakes_from_other_threads_merge_into_one`、`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`；ESP-IDF Host 随 `MA-007` 复核 |
| C-HOST-008 | ISR 不调用任何 Kernel API；ISR 经 Host 提供的 ISR 安全桥接写入事件或最新值寄存器，再由 Host 唤醒所有者线程 | 按键中断服务程序直接调用 `ayni_post_event` | 评审；`MA-007` |
| C-HOST-009 | 存储端口提供 `storage_append`、`storage_read_prefix`、`storage_fence_writers`、`storage_write_snapshot`、`storage_read_snapshot` 与 `storage_free_bytes`；`append` 与 `read_prefix` 成对提供，只有一个时 `ayni_create` 返回 `AYNI_INVALID_ARGUMENT`；`append` 与快照写入以 `OK`、`FAILED`、`UNKNOWN` 之一完成；Host 按批次分帧，`read_prefix` 按日志顺序只返回完整批次：Host 丢弃无法证明已完整写入的尾部批次，证明靠 Host 自己的校验和或写在数据之后的完成标志，单有长度前缀不够；Kernel 仍逐个校验，并把读到的每个批次当作已提交（ADR-0016） | `read_prefix` 返回了提交标记缺失的尾部批次；或 Host 只凭长度前缀判断尾部完整，掉电后交回半个批次，Kernel 进入 `RESTRICTED` | M1 已实现（`append` 与 `read_prefix`：Kernel 侧，以及模拟与 Native Host 的分帧、按日志顺序只返回完整批次、丢弃撕裂的尾部，ADR-0026）：`KernelApi.create_rejects_only_one_storage_port`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`、`HostStorage.reads_follow_log_order_whatever_the_commit_seq`、`HostStorage.only_whole_batches_fit_the_buffer`、`HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.log_file_frames_each_batch_with_two_crcs`；栅栏、快照与 `storage_free_bytes` 端口随 `MA-010` 的 E、F 部分；E2 F7、F8 |
| C-HOST-010 | 存储 Host 只有在批次已按其声明的原子性落到介质、能经受掉电时才报告 `OK`；数据仅进入写缓冲或 Host 队列时不得报告 `OK` | 文件 Host 在 `write()` 返回后、`fsync()` 之前报告 OK | M1 已实现（Native Host：追加在 `fdatasync` 成功后才报告 `OK`，同步失败报告 `UNKNOWN` 且之后的追加都失败，ADR-0026）：评审 `hosts/native/file_log.cpp`，`HostStorage.batches_survive_a_new_host_instance`；Kernel 侧的掉电注入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；C-COMMIT-001 |
| C-HOST-011 | 存储 Host 声明自己是否提供断电原子的替换与 flush；不提供时，Core 使用双槽与提交标记（`storage/records.md`），并用故障注入验证 | ESP32 Host 假定 FAT 的 rename 断电原子，快照切换中掉电后两份都损坏 | `MA-010`；E2 F7 |
| C-HOST-012 | `fence_writers` 以 `DRAINED`（旧 append/flush 全部终止）、`EPOCH_INSTALLED`（新 writer epoch 已原子安装，旧 epoch 写入被拒绝）或 `UNAVAILABLE` 完成；`UNAVAILABLE` 时 Kernel 保持 `RESTRICTED` | Host 在旧 flush 仍在进行时报告 `DRAINED`，重载后旧写落盘使日志分叉 | E2 F12；C-COMMIT-010、C-COMMIT-011 |
| C-HOST-013 | 存储端口报告剩余可用空间；Kernel 在空间不足以保存动作屏障时拒绝新副作用 | 日志分区只剩几个字节，屏障写入失败后才发现，期间副作用已排队 | E2 末段；C-COMMIT-015 |
| C-HOST-014 | 日志出口只接收 Kernel 已脱敏的 trace 记录；Host 不得把凭据、私有记忆正文或媒体写入日志；日志出口满时丢弃并计数，不阻塞 Kernel | 串口日志打印出 Wi-Fi 密码或网关令牌 | 评审；第 10.6 节「安全接入」 |
| C-HOST-015 | Host 不提供的功能显式返回 `AYNI_UNSUPPORTED`；不得用桩函数返回成功 | 没有持久存储的 Host 在 `append` 中直接报告 OK | M1 已实现（存储端口缺失时）：`KernelLifecycle.start_without_storage_is_unsupported_and_stays_configuring`；Native Host 在存储打开之前表中没有存储端口（ADR-0026）：`NativeHost.table_has_storage_ports_only_while_storage_is_open`；`MA-005`；其余 Host 侧随 `MA-007` |
| C-HOST-016 | Host 函数表遵守 C-VER-002/C-VER-003：以 `struct_size` 开头，只在末尾追加；必需端口缺失时 `ayni_create` 返回 `AYNI_INVALID_ARGUMENT` | 缺少时钟的 Host 表被接受，Kernel 第一次计算期限时空指针崩溃 | M0 已实现：`KernelApi.create_rejects_missing_clock`；M1 起另有 `KernelApi.create_rejects_missing_wake_or_ingress_lock`、`KernelApi.host_table_without_0_3_ports_is_invalid_argument` |
| C-HOST-017 | 新 Host 必须通过共享的 Host 契约测试套件后才能标记为已验证；平台差异不得改变本文的语义 | ESP32 Host 的 `append` 在写缓冲时报告 OK，Native Host 在 fsync 后报告 OK，同一测试在两边结果不同 | M1 已实现（存储端口：模拟与 Native Host 通过同一套 `tests/unit/host_storage_contract_test.cpp`，ADR-0026）：`HostStorage.every_accepted_request_completes_once_with_its_own_attempt`、`HostStorage.kernel_boots_on_each_new_host_instance`、`HostStorage.damaged_batch_before_the_last_is_never_dropped`；其余端口的共享套件随各自的包；M2 ESP32 复核 |
| C-HOST-018 | 已受理（`ACCEPTED` 或 `ACCEPT_UNKNOWN`）的异步请求，Host 或 Provider 恰好完成一次，结果未知时以 `UNKNOWN`（Provider 为 `ABANDONED`）完成；全部预留槽完成后 Kernel 才进入 `STOPPED`。组合根在调用 `ayni_destroy` 之前停止所有可能调用 `ayni_post_event`、`ayni_completion_post` 或 `ayni_provider_post` 的线程，这些函数不与 `ayni_create`、`ayni_destroy` 并发 | 停机把一次结果未知的 append 记为未知后进入 `STOPPED`，内存被释放，存储线程随后向已释放的槽写入完成 | M1 已实现（Kernel 侧）：`KernelLifecycle.shutdown_during_recovery_waits_for_the_request_in_flight`、`KernelLifecycle.append_completing_while_draining_is_still_applied`、`WorkJournal.draining_stores_every_batch_before_it_stops`；Native 存储关闭时完成全部已受理请求（ADR-0026）：`NativeHost.closing_storage_answers_every_accepted_request`、`HostStorage.every_accepted_request_completes_once_with_its_own_attempt`；`MA-003`、`MA-005`；其余 Host 侧随 `MA-007` |
| C-HOST-019 | 异步端口引用的 Kernel 缓冲（批次与快照的字节、读取缓冲）在该尝试完成之前由 Kernel 保持有效且不修改，Host 可以从 I/O 线程读写；完成之后 Host 不再访问 | Kernel 在 append 受理后立即复用批次缓冲编码下一个 transition，Host 的 I/O 线程写出新旧混杂的批次 | M1 已实现（恢复路径，同一时刻只有一个存储请求）：模拟 Host 在完成时才读写缓冲，`KernelLifecycle.first_boot_persists_epoch_1_before_running`、`KernelLifecycle.log_longer_than_the_buffer_is_read_in_chunks`；运行中的批次留在环中，追加完成才出环，之前不被覆盖（ADR-0026）：`WorkJournal.transition_waits_for_room_for_its_batch`；Native Host 的 I/O 线程在投递完成之前读写缓冲：`HostStorage.append_then_read_returns_the_same_bytes` |
| C-HOST-020 | `ingress_lock`、`ingress_unlock` 在所有者线程与投递事件、完成的线程之间互斥，可从任意非 ISR 线程调用，不可重入。Kernel 每次持锁只做一步有界操作（一次事件队列操作，每个已入队条目最多读一次，移动的下标少于 `ingress_capacity` 个；或一个完成槽的状态变更），持锁期间不调用任何其他 Host 函数，`wake` 在解锁后调用。Host 不得在自己持有该锁时调用 `ayni_post_event` 或 `ayni_completion_post`（ADR-0015） | Kernel 持锁时调用 `wake`，而 Host 的 `wake` 实现要获取同一把锁，投递完成的线程自锁死 | M1 已实现（完成槽与事件）：`CompletionPoolTest.post_wakes_the_owner_and_no_host_call_runs_under_the_lock`、`CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored`、`CompletionWake.result_is_applicable_when_the_owner_is_woken`、`NativeHost.ingress_lock_excludes_other_threads`、`SimHost.host_calls_under_the_ingress_lock_are_recorded`、`SimHostDeathTest.second_ingress_lock_on_one_thread_aborts`、`KernelLifecycle.first_boot_persists_epoch_1_before_running`（恢复全程没有持锁调用）、`KernelEvents.states_between_start_and_stopped_take_posts`、`KernelEvents.posts_from_host_threads_are_each_taken_once`、`KernelEvents.posts_racing_shutdown_are_each_counted_once`；ESP-IDF Host 随 `MA-007` 复核 |
| C-HOST-021 | 所有者线程在被唤醒、上一次 `ayni_step` 报告 `more_ready` 或到达其 `next_deadline_us` 时再调用 `ayni_step`。Kernel 只为能够推进的定时工作报告期限：放行 Goal 的 occurrence 到期与有界等待结束，以及 Goal 失效，暂停与否（ADR-0019、ADR-0020）；只能由意图或重启解除的等待（Goal 或实例暂停、无界等待的 occurrence 遇到 Task 表满由意图解除，遇到 ID 用尽由重启解除）不设期限，解除它的意图唤醒 owner。`ayni_post_event` 入队后唤醒 owner；本地输入记录的 occurrence 在取出它的 step 开始时到期，它的有界等待同样设期限（ADR-0021） | 暂停的周期目标每次到期都报告期限，Host 反复醒来却无事可做；或 Host 只等唤醒，延时提醒到期后一直不派生 | M1 已实现（Kernel 报告的期限）：`WorkTrigger.delay_goal_derives_once_its_time_comes`、`WorkTrigger.deadline_counts_only_goals_that_could_derive`、`WorkTrigger.paused_goal_takes_what_fell_due_when_resumed`、`WorkTrigger.paused_instance_holds_the_occurrence_until_resumed`、`WorkTrigger.used_up_ids_report_no_deadline`、`WorkTrigger.budget_spent_before_deriving_still_reports_the_due_occurrence`、`WorkTrigger.draining_reports_no_deadline`、`WorkExpiry.goal_expires_at_its_time`、`WorkExpiry.expiry_counts_in_the_deadline`、`WorkExpiry.expiry_needs_neither_room_nor_an_id`、`WorkWait.occurrences_are_skipped_once_their_wait_ends`、`WorkWait.used_up_ids_skip_with_their_reason`、`WorkWait.unbounded_wait_never_skips`、`WorkWait.paused_goal_waits_without_skipping`、`WorkWait.deadline_is_the_end_of_the_earliest_bounded_wait`、`WorkExpiry.clock_at_its_largest_value_finds_no_due_work`、`WorkInput.goal_takes_inputs_from_its_source`、`WorkInput.inputs_waiting_too_long_are_skipped`；按期限睡眠的 step 循环随 E1 夹具与 `MA-007` |
