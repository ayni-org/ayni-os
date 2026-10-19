# ID 与时间

- **关联**：计划 §2.2、§2.7、§3.4、§3.6、§4.4；ADR-0004、ADR-0017；`MA-003`、`MA-004`、`MA-038`；E2 F9、F13，E3 步骤 2
- **属主**：`core/foundation`（类型与生成规则），各 ID 的分配者见下表

## ID 一览

| ID | 分配者 | 唯一范围 | 跨重启 | 用途 |
|---|---|---|---|---|
| `device_id` | 绑定时由所有者流程签发 | 全局（所有者范围内） | 稳定 | 设备身份 |
| `boot_epoch` | Kernel 启动或从受限栅栏重载时递增并持久化 | 每设备 | 每次启动/重载变化 | 区分请求、结果和订阅的代际 |
| `agent_id` | 注册时 | 所有者范围 | 稳定 | 逻辑 Agent 身份 |
| `instance_id` | Kernel | 每设备 | 稳定 | 驻留实例 |
| `goal_id`、`task_id` | Kernel | 每设备 | 稳定 | 持续目标与一次有界执行 |
| `session_id` | Kernel，在 Host 打开会话时（`ayni_open_session`） | 每设备 | 不跨启动 | 一个输入渠道上的主体会话（C-AUTH-010） |
| `step_key` | 策略或 GoalType | 每 Task | 跨策略替换稳定 | 策略可见的步骤名 |
| `logical_call_id` | Kernel，在首次接受 `(task_id, step_key)` 时 | 每设备 | 稳定 | 语义调用身份 |
| `operation_id` | Kernel，在准备记录中 | `(已认证调用主体, 控制域 authority-owner)` | 稳定 | 有副作用调用的幂等键 |
| `request_id` | Kernel | 每 `boot_epoch` | 不跨启动 | 一次异步 I/O 尝试 |
| `command_id` | 管理端 | `(principal_id, target_device_id)` | 稳定 | 管理命令幂等键 |
| `message_id` | 发送方 | 每条传输消息 | 不承担业务含义 | 传输去重与追踪 |
| `transition_id`、`commit_seq` | Kernel | 每设备日志 | 稳定 | 提交批次与持久前缀 |
| `subscription_id`、`generation` | Kernel | 每设备 | 订阅 ID 稳定，代际每次重新准入递增 | 订阅与旧尾部隔离 |
| `permit_id` | 控制域 authority-owner | 每控制域 | 不跨 `kernel_boot_epoch` 有效 | 执行许可 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-ID-001 | 稳定 ID 一经分配不复用，即使对象已删除或终结；删除只留 tombstone | 删除 Goal 后新 Goal 取得相同 `goal_id`，旧回执把新 Goal 标为完成 | M1 已实现（Kernel 分配的 ID 不复用；对象尚未持久化，重启后不恢复）：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`；删除与 tombstone 随 `MA-010`；E1 步骤 9 |
| C-ID-002 | Core 内部以定长整数表示 ID；需要文本形式时（日志、JSON）按 C-JSON-004 以十进制字符串表示 64 位值 | 64 位 ID 以 JSON 数字传给 JavaScript 客户端后丢失精度 | `wire/json_subset.md`；M1 编解码测试 |
| C-ID-003 | `logical_call_id` 与 `(task_id, step_key)` 一一对应，在首次接受该步骤时与步骤检查点同批持久化；重启、重连和策略替换都不重新生成 | 换策略后同一步骤得到新 `logical_call_id`，已完成的动作被当成新动作再做 | E3 步骤 2–4；E4 步骤 2 |
| C-ID-004 | `operation_id` 与 `(logical_call_id, binding_id)` 一一对应；只有取得原 operation 的权威未执行结论并封禁旧 ID 后，才能为同一逻辑调用换绑定并分配新 `operation_id` | 结果未知时换一个 Provider 并用新 ID 重做 | E2 F5；E3 步骤 2；`C-CALL-008` |
| C-ID-005 | `operation_id` 取自每个调用主体单调递增的序号；接收方回收去重记录时保留每主体低水位，低于低水位且无记录的请求返回 `EXPIRED`；去重记录的保留期不短于协商的最长重试期 | 去重记录回收后，迟到的旧请求被当作新动作执行 | `MA-038`；ADR-0004 |
| C-ID-006 | `request_id` 只在一个 `boot_epoch` 内唯一；结果以 `(boot_epoch, request_id)` 关联，旧代结果不得修改新状态。恢复请求以 `boot_epoch` 0 命名，`request_id` 随每次 `ayni_create` 重新开始，所以尝试只在一个 Kernel 实例内唯一；Host 不得把上一个实例发出的请求的结果投递给新实例（ADR-0016） | 重启前发出的请求在重启后返回，被应用到新请求上 | M1 已实现（完成槽）：`CompletionPoolTest.post_from_an_earlier_boot_is_ignored`、`CompletionPoolTest.late_post_cannot_complete_a_reused_slot`；恢复请求以 `boot_epoch` 0 命名且 `request_id` 依次递增：`KernelLifecycle.recovery_requests_use_boot_epoch_0`；Host 不投递旧实例结果的契约测试随 `MA-010`；E2 F7；E3 步骤 2 |
| C-ID-007 | `message_id` 每次重试可以改变，不得用作业务幂等键；业务幂等只看 `command_id` 或 `operation_id` | 以 `message_id` 去重，重连重发的同一命令创建第二个提醒 | E2 F9 |
| C-ID-008 | 一条消息只携带一种幂等键：管理命令用 `command_id`，设备间调用用 `operation_id` | 一条 `capability.invoke` 同时带两个键，二者冲突时无法判定 | `wire/envelope.md` C-WIRE-003 |
| C-ID-009 | `boot_epoch` 在 Kernel 开放任何输出之前已持久化递增；从受限栅栏重载也视为新的启动 | 重载后沿用旧 `boot_epoch`，旧写入方的完成事件被当作当前代际 | M1 已实现（启动）：`KernelLifecycle.recovery_lasts_until_the_epoch_append_completes`、`KernelLifecycle.each_reboot_persists_the_next_epoch`、`KernelLifecycle.storage_faults_restrict_with_their_reason`、`KernelLifecycle.reboot_after_an_unknown_append_skips_its_epoch`；受限栅栏重载随 `MA-010`；E2 F12 |
| C-ID-010 | Kernel 分配的 `instance_id`、`goal_id`、`session_id` 共用一个分配器，取值为 `boot_epoch × 2^32 + seq`：高 32 位是本次启动已持久化的 `boot_epoch`，`seq` 从 1 开始逐个递增，每次启动最多 2^32−1 个，用尽后新的分配返回 `CAPACITY_EXCEEDED`（`ID_EXHAUSTED`），不回绕；`boot_epoch` 因此以 2^32−1 为上限，日志中的最后一个代际已到上限时启动进入 `RESTRICTED`（`BOOT_EPOCH_EXHAUSTED`）。ID 0 不指向任何对象。不同启动分配的 ID 因代际不同而不相等，不需要持久化计数器（ADR-0017） | 每次启动的序号从 1 开始而不带代际，重启后新 Goal 取得旧 Goal 的 `goal_id` | M1 已实现：`KernelId.ids_carry_the_boot_epoch_and_a_sequence_from_1`、`KernelId.allocator_runs_out_instead_of_wrapping`、`WorkModel.used_up_ids_refuse_goals_and_sessions`、`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work`、`KernelLifecycle.largest_epoch_and_transition_id_still_boot`；实例跨重启保持同一 `instance_id` 随 `MA-010` |

## 时间

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-TIME-001 | 本机期限（TTL、deadline、租期、等待上限）一律用 Host 提供的单调时钟（微秒，`monotonic_now_us`）计算 | 用墙上时钟计算动作 deadline，校时后动作提前或延后数小时 | `MA-003` FakeClock 测试 |
| C-TIME-002 | 单调时间值不持久化为可恢复的期限，不跨设备传递或比较；跨设备只传相对时限与可信时间信息 | 把对端的单调时间戳与本机时间相减判断新鲜度 | 评审；`MA-038` |
| C-TIME-003 | 持久记录保存期限的恢复策略（如“重启后重新计时”或“依赖绝对时间”），不保存旧的单调数值 | 重启后用重启前的单调时间戳判断 Observation 仍然有效 | E2 F1；`MA-010` |
| C-TIME-004 | 绝对时间不可信时，依赖日历或绝对期限的工作进入待校时状态，不按猜测的时间执行 | 断电重启后以 1970 年时间触发全部提醒 | `MA-011` 日历边界测试 |
| C-TIME-005 | 日历目标保存时区与规则版本，以计划发生时刻形成稳定 occurrence ID；同一 occurrence 只触发一次 | 夏令时回拨的重复一小时内同一提醒触发两次 | E1 步骤 11；`MA-011` |
| C-TIME-006 | 夏令时缺失时段跳到下一个有效时刻；向后校时不重新触发已提交 occurrence；向前跨过时刻按目标的错过策略处理 | 向后校时一小时，已呈现的提醒再次播报 | `MA-011` |
| C-TIME-007 | 单调间隔（“每两分钟”）与日历规则（“每天 14:00”）是两种类型，不互相换算 | 把“每天 14:00”实现为每 86,400 秒触发 | `MA-011` |
| C-TIME-008 | `ayni_host_v1.monotonic_now_us` 在一次启动内不减小，不阻塞，并在 Kernel 所有者线程上调用 | Host 返回墙上时钟，校时后时间倒退 | M0 已实现：`NativeHost.monotonic_clock_never_decreases`（Native）；ESP32 使用 `esp_timer_get_time`，M2 补测 |
