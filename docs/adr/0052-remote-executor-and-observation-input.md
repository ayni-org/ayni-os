# ADR-0052：远程执行端与观测输入的线程模型

- **状态**：已接受（用户 2026-10-26 选定“现在定义异步执行端 ABI”，取代原推荐的本地代理闸门；观测输入维持原推荐，随 MA-008B 实现）
- **日期**：2026-10-25 提议；2026-10-26 接受并实施
- **关联**：ADR-0015、ADR-0044、ADR-0049、ADR-0050、ADR-0051；架构静态审查（基线 `e089a22`）F15；`contracts/domain/permits_and_stop.md`（C-PERMIT-002/005/006、C-STOP-006）；MA-008B 订阅

## 背景

Core 的外部 I/O 原有两种线程模型并存：

- Provider 走异步邮箱（ADR-0044）：`submit` 只受理，结果由任意线程投递，拥有者线程消费。
- 执行端端口（`include/ayni/execution.h`，ABI 0.35）在拥有者线程同步、有界、非阻塞地运行：派发前 `challenge`，续租时 `challenge` 与 `renew`，急停时 `stop`，轮询时 `status`，均在 `ayni_step` 内由 `ExecutionControl` 调用。M1 只有本地 Mock。
- `ayni_publish_observation` 只能在拥有者线程调用；事件则经线程安全的 ingress 队列（ADR-0015）。

真实或远程执行端的 nonce 往返要经过链路，同步端口要么阻塞 step，要么给不出新鲜 nonce；MA-008B 订阅的数据源大多在其他线程。提议稿推荐用本地代理闸门保持同步 ABI，用户选择直接把执行端端口改为异步请求。

## 决策

1. **四个执行端端口都是请求（ABI 0.36）。** `challenge`、`renew`、`stop`、`status` 各带一个请求身份 `ayni_attempt_ref_v1` 和一个完成目标 `ayni_executor_completion_t *`。端口返回 `AYNI_OK` 表示收下请求，之后恰好有一次 `ayni_executor_post` 回答它，可以在端口内同步投递，也可以来自任意非 ISR 线程；返回其他状态表示拒绝，Core 把返回值当作这次请求的回答，之后的投递被拒。回答 `ayni_executor_response_v1` 带请求种类、请求身份、状态和输出：`CHALLENGE` 成功时带 nonce，`STATUS` 成功时带执行端状态，其他情况的输出被丢弃。
2. **执行端邮箱。** Kernel 为每个执行端预留三条请求通道，放在 arena 的 `executor_completions` 区：控制通道（派发与续租的 `challenge`、`renew`）、停止通道、状态通道。每条通道同时只有一个未回答的请求。投递在 ingress 锁内校验并保存回答，解锁后唤醒 Host，与 Provider 邮箱同一纪律（ADR-0044、C-HOST-020）。请求身份取自 Provider attempt 所用的同一序列，迟到的回答不能冒充新请求（C-ID-006）。结构大小、种类、保留字段或目标不合法时返回 `INVALID_ARGUMENT`；通道已不在等这次请求（迟到、重复、种类或身份不符）时返回 `INVALID_STATE`，什么都不改。
3. **等待有上限，step 不等待执行端。** 每个请求至少等待 `poll_period_us`（仍不长于最短租期）；到期后 Core 处理它时仍没有回答，记为 `AYNI_RESULT_UNKNOWN` 并关闭通道，之后的投递被拒。一个工作项要么发出一个请求，要么应用一个回答，不在发出请求的同一项里应用它的回答。未到期且未回答的请求不算待办工作，`ayni_step` 不会为它空转。
4. **派发在持久屏障后等待挑战。** `DISPATCHING` 屏障持久后发出 `challenge`，调用停在屏障阶段。回答到达后的那一项重新用新时间复核授权、资源和执行条件，再用 nonce 安装许可、提交 Provider；在此之前没有任何东西到达 Provider。挑战被拒（含 `BUSY`）、nonce 无效或超时，调用在派发前以本地取消结束，不重试，避免对忙碌的执行端反复发请求。调用在等待期间结束时关闭它的控制通道。同一执行端已有许可或其他调用的挑战在途时，调用照旧等待。
5. **停止链在异步回答下重新论证，规则不放松。**
   - **急停优先**：`advance_urgent` 仍在日志准入之前运行，停止请求走独立通道，在途的续租或状态读取不会推迟它；停止请求到期未回答时也在急停轮次取回并重发。
   - **撤销即停续租**：续租是一条链。状态读到 `ACTIVE` 才发 `challenge`；挑战的回答在之后某一项到达，该项重新判断是否应停止，仍允许才发 `renew`。所以续租回显的 nonce 由执行端在 Core 最后一次判定允许之前签发，撤销发生在这次判定之后，续租得到的截止不晚于“撤销时刻 + `L`”，与 C-PERMIT-005/006 的界相同。撤销所在的那一项就请求停止，不等在途的挑战。
   - **停止证据不变**：只有 `STATUS` 回答里的停止测量是停止证据；`stop` 的回答无论成功或拒绝都不是。停止请求超时没有回答时重发；被明确拒绝时不重发，由执行端自身的租约收敛，与 0.35 的端口失败处理一致。状态或控制通道的拒绝、无效输出或超时都请求停止。
   - **`NOT_FOUND` 的时序**：只有在 Core 看到 Provider 终态之后才发出的状态读取答 `NOT_FOUND`，才证明许可从未开始；终态之前发出、之后才答的读取不作数，再读一次。远程闸门可能晚于 Provider 受理才收到许可，所以执行端答 `NOT_FOUND` 后必须拒绝该许可此后再开始；为此，许可交给 Provider 后的第一次状态读取等一个轮询周期再发。
   - 资源隔离直到停止事实持久才解除；许可释放时关闭该执行端的三条通道，之后的回答被拒。
6. **观测输入维持原推荐。** 拥有者线程之外的观测源经与事件相同的线程安全队列进入拥有者线程，再按现有规则写入 `ObservationTable`；`ayni_publish_observation` 保留为拥有者线程接口。队列满时拒绝新样本并计数，不阻塞生产者；丢失的样本只会让 Scene 依赖变旧而停止执行。这部分在 MA-008B 实现，本次不改。
7. **Mock 执行端演示远程语义。** 它可以在端口内立即回答，也可以留到 `answer()` 或用 `drop_requests()` 丢弃（模拟慢链路或断链）；许可可以在 submit 时或下一次 `advance()` 才到达闸门（模拟 Provider 转发）。答过 `NOT_FOUND` 的许可此后被拒。

## 后果

- ABI 次版本升到 0.36，执行端、Host、SDK 与 Mock 统一重编译；能同步回答的执行端只需在端口内投递。storage schema 2 与持久记录不变。
- 远程执行端不再需要本地代理闸门，但往返时间必须短于 `poll_period_us`，否则每个请求都按未知处理；Provider 也须在一个轮询周期内把许可转发到闸门，否则第一次状态读取答 `NOT_FOUND`，许可被拦下，调用以停止结束（安全，但调用失败）。
- 续租从一次同步调用变为四个工作项（发出状态读取、应用状态并发出挑战、应用挑战并发出续租、应用续租），跨三次往返；第一次续租还要等派发挑战的往返和一个轮询周期。会话租期 `L` 的声明要覆盖这些时间和漂移余量。
- 派发多一个等待阶段，调用在屏障与挑战回答之间不占用 step；挑战 `BUSY` 由 0.35 的“稍后重试”改为结束调用。
- 内存：默认、最小和调用配置的 Kernel 对象各增加 40 B（邮箱与引用）；执行配置（一个执行端）增加 496 B，即这 40 B 加三条通道共 456 B。
- 停止链只在 Core 与 Mock 范围内重新论证；真实链路、ESP32、停止时限与重启/分区矩阵仍未验证（`MA-026`、`MA-038`）。

## 备选方案

- **本地代理闸门，执行端 ABI 不变**（提议稿的推荐）：每个远程执行端多一个代理，并要另立代理与远端之间的一致性规则；用户未选。
- **暂不决定，只记约束**：同步语义会继续被新代码依赖，之后迁移的成本更高。
- **只把 `challenge`、`renew` 异步化，`stop`、`status` 保持同步**：远程停止与状态读取仍要么阻塞 step，要么只能给出代理侧的结论，不采用。
- **每个执行端一条请求队列**：在途的续租或状态读取会推迟停止请求，违背急停优先，不采用。
- **允许任意线程调用 `ayni_publish_observation`**：`ObservationTable` 要加锁，并与拥有者线程上的 Scene 判断竞争，违背单一拥有者模型。

## 验证

- ABI 布局：`tests/contract/abi_layout.c` 固定 `ayni_executor_output_v1`（64 B）与 `ayni_executor_response_v1`（96 B）的大小和字段偏移。
- 邮箱：`ExecutorMailboxTest.one_answer_per_request_and_late_or_duplicate_posts_are_refused`（每个请求一次回答，迟到、重复与旧地址的投递被拒，投递唤醒 Host）、`ExecutorMailboxTest.a_stop_lane_answers_while_other_lanes_wait`、`ExecutorMailboxTest.malformed_answers_and_foreign_targets_change_nothing`。
- Core 正常路径：既有 `ManagedExecution` 用例改用异步端口后全部通过（Mock 在端口内回答）。
- 延迟与断链：`ManagedExecution.held_challenge_waits_at_the_barrier_without_spinning`、`ManagedExecution.unanswered_challenge_times_out_before_any_permit`、`ManagedExecution.lost_link_ends_output_by_the_executors_own_lease`、`ManagedExecution.revocation_while_a_renewal_challenge_is_held_sends_no_renewal`、`ManagedExecution.open_requests_wake_a_host_that_sleeps_until_the_reported_deadline`（只在 `more_ready` 或报告的截止时间才 step 的 Host 也能等到停止重发）。
- `NOT_FOUND` 时序：`ManagedExecution.not_found_fences_a_permit_still_on_its_way_to_the_gate`、`ManagedExecution.not_found_proves_not_started_only_from_a_read_after_the_terminal`、`ManagedExecution.a_permit_forwarded_within_a_poll_period_starts`。
- 未验证：真实远程链路与时限、执行端或 Kernel 重启与分区下的请求、观测队列（MA-008B）。
- 实际命令、内存测量、CI 与独立审查见 [#71](https://github.com/RobotSe7en/ayni/pull/71)。
