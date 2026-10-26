# 执行许可与停止

- **关联**：计划 §2.2、§2.4「紧急停止」、§2.10、§4.1、§7.3；ADR-0001、ADR-0005；`MA-018`、`MA-026`、`MA-038`；E2 F12、F14，R1 A2–A4、B5
- **属主**：`resources/permit`（签发、续租、失效判断，在 authority-owner Kernel）；`control/`（执行端许可门与包络校验）；`safety/`（独立停止链）。`control/` 与 `safety/` 不依赖 `core/`（ADR-0005、ADR-0011）。

当前实施边界（ABI 0.36，ADR-0049/0050/0052）：独立 `control/permit_gate` 与 Core 的 Scene、观察前提、持久许可签发/会话续租及停止事实已组成 Native Mock 调用链；执行端端口自 0.36 起是异步请求，回答由任意线程投递。物理 action 须注册精确 ExecutionBinding；默认配置不启用这些容量。仅验证本地可信端口、Mock 的延迟与断链及正常场景；真实硬件、远端链路与认证、停止时限及新故障/恢复专项未验证。

## 角色

| 角色 | 承担者 | 职责 | 不承担 |
|---|---|---|---|
| 决策点 | 控制域的 authority-owner Kernel | 静态准入、动作账本、许可签发与续租 | 不进入实时环路 |
| 执行点 | 本机控制器、托管设备端点、停止监督器 | 每个控制周期校验许可、限值与持续联锁；负责停止 | 不授予或扩大权限 |

两种执行形态：过程（Procedure）是一次 `action` 调用，由本地技能在声明的包络、最长时长与停止条件内完成；控制会话（ControlSession）在获准包络内接收流式指令。二者都在过程或会话粒度使用 ActionRecord 与派发屏障；包络内的样本属于该次获准执行的内部过程，不逐条成为业务 operation。

## 许可字段

| 字段组 | 字段 | 规则 |
|---|---|---|
| 身份与范围 | `permit_id`、`operation_id`、控制域、控制模式、完整数值包络 | ABI 0.34 携带最多 8 个字段及单位/闭区间，逐项比较，不只凭摘要（ADR-0049，用户已确认） |
| 代际 | `kernel_boot_epoch`、`authority_epoch`、`executor_boot_epoch`、`safety_epoch`、校准版本 | 任一项与执行端当前值不一致即失效 |
| 时效 | 租期 `L`（由能力声明） | 会话为需续租的短租约；过程许可不续租，时长不超过该过程的最长执行时长 |
| 新鲜度 | 回显的执行端 nonce | 见下节 |

## 截止时间的计算

执行端按本地单调时钟记录每个 nonce 的签发时刻 `t_N`，周期轮换 nonce 并只保留最近 `K` 个。许可或续租回显 nonce N 时：

```
deadline = max(deadline, t_N + L)
```

截止以 nonce 的签发时刻为锚，不以许可到达时刻为锚，传输延迟不能延长截止。未知或已淘汰的 nonce 一律拒绝。撤销前签出的续租所回显的 nonce 必然签发于撤销之前，因此任何在途续租都不能把截止推过“撤销时刻 + `L`”。`L` 的取值须包含执行端与 Kernel 单调时钟的漂移余量。

E2 F14 的在途续租用例：原截止 100 ms，91 ms 签出续租，92 ms 撤销，95 ms 续租到达。为便于计算，设 `L` = 10 ms，且该续租回显的 nonce 由执行端在 89 ms 签发（两者都是示例假设）。新截止为 `max(100, 89 + 10) = 100` ms，不晚于撤销时刻 + `L` = 102 ms；若按到达时刻计算则会得到 105 ms，这正是本规则禁止的。

## 失效矩阵

| 失效事件 | 失效机制 | 最坏停止上限（均须实测） |
|---|---|---|
| Grant 撤销或场景关闭，撤销送达 | 显式撤销，执行端立即收敛 | 传输时间 + 停止时间 |
| 撤销传播失败 | Kernel 内存撤销栅栏先停止续租；在途续租的截止不晚于其 nonce 签发时刻 + `L` | 撤销时刻 + `L` + 停止时间 |
| Kernel 重启、崩溃或进入受限栅栏 | 不再续租；新 `kernel_boot_epoch` 的续租被拒，须重新准入 | 失效时刻 + `L` + 停止时间 |
| 执行端或控制器重启 | `executor_boot_epoch` 变化，启动默认失能 | 由启动禁用保证 |
| 校准失效 | 执行端本地校准版本变化 | 检测时间 + 停止时间 |
| 紧急停止 | 硬件失能与停止锁存 | 停止链分项实测时间 |
| 急停复位 | `safety_epoch` 递增，旧许可永久失效；须所有者显式恢复并重新准入 | — |
| 换主或网络分区 | 执行端持久化的 `authority_epoch` 递增后旧 owner 的续租被拒；分区期间无法续租 | 换主或分区时刻 + `L` + 停止时间 |

## 停止链（ESP32）

| 层 | 机制 | 依赖 | 覆盖的失效 |
|---|---|---|---|
| L0 硬件切断 | 急停直接切断驱动器使能，或接入 MCPWM GPIO fault 并使用需软件恢复的 OST 制动；使能脚外部下拉，复位与启动期间默认失能 | 不依赖固件 | 固件任何状态、复位、上电 |
| L1 硬件超时 | 外部看门电路：使能须由持续翻转的信号维持，翻转停止超过 `T_hw` 即失能 | 信号按时翻转 | L2 停止翻转，或固件整体失效 |
| L2 IRAM 监督 | IRAM-safe 定时中断仅在许可有效且控制任务心跳在 `T_hb` 内时翻转 L1 信号；代码与数据只在 IRAM/DRAM | 中断与内部 RAM 可用 | 许可过期、控制任务心跳丢失（含 Flash 操作造成的挂起） |
| L3 控制任务 | 在包络内减速停止并上报停止事实 | 任务调度 | 正常停止、包络越界 |

| 故障 | 触发停止的依据 | 最坏停止上限（分项实测） |
|---|---|---|
| 控制任务失效，或 Flash 操作造成长时间挂起 | 控制心跳超过 `T_hb`，L2 停止翻转 | `T_hb + T_hw + 驱动器关断 + 机械停止` |
| Kernel 失效、撤销传播失败、受限栅栏、链路中断或分区（控制任务正常） | 许可越过截止：L3 收敛，L2 同时停止翻转作为上界 | 失效时刻 + `L` + `T_hw` + 驱动器关断 + 机械停止 |
| 紧急停止 | L0 硬件切断 | L0 切断时间 + 驱动器关断 + 机械停止 |
| 固件整体失效或复位 | L1 硬件超时 | `T_hw + 驱动器关断 + 机械停止` |

各行独立成立，不要求 `L` 与 `T_hb` 之间存在大小关系。候选电路见 `docs/hardware/stop-chain.md`。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-PERMIT-001 | 每个控制域只由其当前 authority-owner Kernel 签发许可；执行端拒绝其他来源或旧 `authority_epoch` 的许可 | 非 owner 的 Kernel 向底盘控制器签发许可并被接受 | `MA-018`；`MA-038`（换主与分区） |
| C-PERMIT-002 | 许可只在 `DISPATCHING` 屏障已持久（`durable_seq` 覆盖屏障）且屏障后复核通过后签发；签发与续租都是带 `requires_seq` 的对外输出 | 屏障还在写入时就签发许可，屏障写失败但电机已开始转动 | 正常屏障：`ManagedExecution.dispatch_waits_for_storage_and_stopped_fact_precedes_resource_reuse`；E2 F2、F10；C-COMMIT-006、C-COMMIT-007 |
| C-PERMIT-003 | 执行端只能收紧：许可包络 ⊆ 准入时批准的包络 ⊆ 设备限值；执行端不得授予或扩大权限；扩大包络、改变控制模式或权限须重新准入并记录 | 执行端收到超出设备限值的速度指令时按限值“截断后照做”，并把会话包络改为更大的值 | 本地门：`ControlGate.incomplete_or_enlarged_envelopes_are_rejected_before_start`、`ControlGate.an_out_of_bounds_sample_stops_instead_of_clamping`；Core 正常准入：`ManagedExecution.procedure_finishes_only_with_independent_stop_and_releases_actuator`；R1 A2；C-AUTH-016 |
| C-PERMIT-004 | 许可携带上表全部代际；执行端每个控制周期比对 `kernel_boot_epoch`、`authority_epoch`、`executor_boot_epoch`、`safety_epoch` 与校准版本，任一不一致即按包络声明的方式收敛停止 | 控制器重启后接受了重启前签发的许可 | E2 F14（Kernel 重启、校准失效、急停复位后重放旧许可） |
| C-PERMIT-005 | 撤销 Grant 或关闭场景时，Kernel 在内存撤销栅栏建立的同一 transition 中停止续租受影响的许可，不等待撤销持久化完成；撤销消息传播失败时，执行端最迟在撤销时刻 + `L` 后收敛 | 撤销等 Flash 写完才停止续租，期间又续租了两次 | 正常撤销/关闭：`ManagedExecution.grant_revocation_stops_without_waiting_for_durable_cleanup`、`ManagedExecution.session_renews_with_fresh_nonce_and_normal_close_never_resumes_old_motion`；E2 F14（撤销传播失败；在途续租时序）；C-AUTH-006 |
| C-PERMIT-006 | 执行端按“截止时间的计算”一节更新截止：`deadline = max(deadline, t_N + L)`，`t_N` 为执行端本地签发 nonce 的时刻；未知或已淘汰的 nonce 拒绝 | 执行端以续租到达时刻加 `L` 作为新截止，网络延迟 500 ms 的续租把截止推迟了 500 ms | 正常本地门：`ControlGate.lease_anchors_at_nonce_and_renewal_never_uses_arrival_time`、`ControlGate.unknown_evicted_and_expired_challenges_do_not_start_execution`；E2 F14 传播专项待验收 |
| C-PERMIT-007 | 执行端拒绝任何缺少有效许可的指令，无论来源；每个控制周期检查代际一致、本地单调时间未超过截止、未处于停止锁存、校准有效、指令在包络内且年龄不超过上限，任一项不满足即收敛停止 | 绕过 Kernel 直接发给控制器的速度指令被执行 | 本地门：`ControlGate.startup_requires_a_permit_and_first_sample_before_output`、`ControlGate.control_cycles_expire_the_last_sample_without_waiting_for_lease_end`、`ControlGate.missing_duplicate_wrong_unit_and_stale_samples_stop_the_permit`、`ControlGate.epoch_and_interlock_changes_do_not_allow_output_or_renewal`；执行器/独立停止证据待验收；C-TRUST-004 |
| C-PERMIT-008 | 越过截止即为终态：会话或过程停止后，之后到达的任何许可或续租都不能恢复它；恢复运动须以新 `permit_id` 重新准入；资源在停止确认前保持隔离（C-RES-008） | 截止后 5 ms 到达的续租让已停止的会话重新开始运动 | 本地终态：`ControlGate.renewed_sessions_cannot_resume_after_the_exact_deadline`、`ControlGate.procedure_is_nonrenewable_and_expiry_is_permanently_terminal`；Core 正常到期/隔离：`ManagedExecution.procedure_expires_normally_without_renewal_and_never_revives`、`ManagedExecution.unconfirmed_stop_retains_terminal_and_quarantine_blocks_other_agent`；E2 F14 专项待验收 |
| C-PERMIT-009 | Kernel 处于受限栅栏、`RESTRICTED`、`DRAINING` 或正在重启时不签发、不续租任何许可 | 存储写失败进入受限栅栏后，许可仍按周期续租，电机一直转 | E2 F12（模拟执行端在失效时刻 + `L` 内停止）；C-COMMIT-009 |
| C-PERMIT-010 | 会话租期 `L` 由能力声明，按能力风险与实测停止时间设定，并包含时钟漂移余量；过程许可时长不超过过程的最长执行时长且不续租 | 过程许可设为 10 分钟并可续租，撤销后电机最长还能转 10 分钟 | `MA-026` 停止上限实测；评审 |
| C-PERMIT-011 | 无法给出停止上限的能力不开放会话形态；能力只有在适用于它的最坏停止上限满足其运动限值时才可开放 | 没有实测停止时间的底盘能力开放了遥操作会话 | `MA-026` |
| C-PERMIT-012 | 执行端或控制器启动时默认失能，`executor_boot_epoch` 变化；启动后必须收到新签发的许可才能输出 | 控制器复位后按复位前最后的 PWM 占空比继续输出 | E2 F14；`MA-026` |
| C-PERMIT-013 | 急停复位使 `safety_epoch` 递增，所有旧许可永久失效；恢复运动须所有者显式恢复并重新准入 | 急停复位后，之前签出的许可又让电机转动 | E2 F14（急停复位后重放旧许可） |
| C-PERMIT-014 | 包络内的高频样本属于该次获准执行的内部过程，不逐条写日志、不逐条成为业务 operation；样本缓冲有界 | 遥操作的每个速度样本都写一条 ActionRecord，日志在数秒内写满 | R1 A3；`MA-035` |
| C-PERMIT-015 | 跨设备控制时，全部许可与新鲜度校验在执行端所在节点完成；新鲜度只由执行端 nonce 与本地租期证明，无法证明则拒绝，不以“首次收到后 TTL”替代 | 在网络中滞留 3 秒的许可被执行端当作刚签发的许可接受 | `MA-038`（首次到达长延迟、分区下许可到期） |
| C-PERMIT-016 | 静态准入（C-AUTH-016）由 Kernel 完成；执行期间的持续联锁（反馈丢失、堵转、倾斜、越界、断流、控制者失联）由执行端声明并逐控制周期执行，任一触发即按包络声明的方式收敛停止，不等待 Kernel；独立停止覆盖所有控制者 | 堵转检测放在 Kernel 的周期任务中，Kernel 被 Flash 写入挂起期间电机持续堵转 | `MA-018`；`MA-026` 故障矩阵 |
| C-PERMIT-017 | 请求中的运动配置名（如 `calibrated-short-turn`）只是引用：执行端把它解析为本机实测并已启用的具体参数上限，解析不到、未启用或校准失效时拒绝；配置名本身不是校准证据或授权 | 新设备从未标定，却因收到 `calibrated-short-turn` 而按默认参数转动 | `MA-026`（底盘校准）；`MA-038` |
| C-PERMIT-018 | 执行端请求的回答只应用一次，且不在发出该请求的工作项内应用；每个请求至少等待 `poll_period_us`，到期后仍未回答记为结果未知，迟到的回答被拒；派发的挑战回答到达后，Core 用新时间重新复核授权、资源与执行条件才安装许可，挑战被拒、nonce 无效或超时都在派发前结束调用（ADR-0052） | 链路恢复后才送达的 3 秒前的挑战回答被 Core 接受，其 nonce 签出许可并提交给 Provider | `ExecutorMailboxTest.one_answer_per_request_and_late_or_duplicate_posts_are_refused`、`ExecutorMailboxTest.malformed_answers_and_foreign_targets_change_nothing`、`ManagedExecution.held_challenge_waits_at_the_barrier_without_spinning`、`ManagedExecution.unanswered_challenge_times_out_before_any_permit`；真实链路待 `MA-038` |
| C-STOP-001 | 停止路径不依赖普通事件队列的空位；停止监督器的安全状态是电平，用可覆盖的最新值寄存器（停止锁存 + `safety_epoch`）表达，不进入队列 | 事件队列满时，急停事件被丢弃或排在 128 个遥测事件之后 | E1 步骤 10；`MA-003` |
| C-STOP-002 | 紧急停止由独立本地入口立即禁止运动准入，再向 Core 报告；它不取消提醒、不删除目标；电机保持失能，直到故障清除且所有者显式恢复；解除暂停不解除停止锁存 | 按下急停后，“解除暂停助理”就让底盘恢复运动 | E1 步骤 11；`MA-026` |
| C-STOP-003 | L0：急停直接切断驱动器使能，或经 MCPWM fault 触发需软件恢复的 OST 制动；使能脚有外部下拉，复位与上电期间驱动器失能，不依赖固件 | 使能脚悬空，ESP32 复位期间电机短暂转动 | `MA-001` 电路审查；`MA-026` 使能脚实测 |
| C-STOP-004 | L1：驱动器使能由持续翻转的信号维持，翻转停止超过 `T_hw` 即失能；`T_hw` 由电路参数决定并实测 | L1 用电平而非翻转维持使能，固件卡死时输出保持高电平，电机不停 | `MA-001` 电路审查；`MA-026` |
| C-STOP-005 | L2：IRAM-safe 定时中断只在许可有效且控制任务心跳在 `T_hb` 内时翻转 L1 信号，代码与数据只驻留 IRAM/DRAM；`T_hb` 大于实测最长 Flash 页编程时间；日志的扇区擦除在无活动执行器会话时预先完成，会话期间只做页编程 | 监督代码位于 Flash，Flash 擦除期间中断无法运行，L1 超时误停 | `MA-026` 故障矩阵（运动中 Flash 擦写）；C-STORE-012 |
| C-STOP-006 | 取消请求已受理、停止指令已送达、物理输出已停止（停止确认）是三种事实，分别记录；停止确认须有本机测量证据（使能脚状态、编码器/IMU 或控制器输出状态），模型推断或远端观察不能替代 | 视觉模型判断“画面不动了”，系统据此报告电机已停止 | 正常 Mock：`ManagedExecution.unconfirmed_stop_retains_terminal_and_quarantine_blocks_other_agent`；E1 步骤 4；R1 A4；C-LIFE-012 |
| C-STOP-007 | 运动结果分别记录命令已接受、执行结束、停止确认与目标验证（`VERIFIED`、`ESTIMATED`、`UNKNOWN`），附传感来源、实测或估计、时刻与容差；没有角度或位姿反馈的限时动作只报告执行时长、停止事实与估计结果 | 开环限时转向结束后报告“已转动 30°（实测）” | R1 A4；`MA-026` |
| C-STOP-008 | 最坏停止上限按上表的故障类型分别预算并逐项实测；v0.1 只声明“在已测故障列表下的实测停止上限”，不声明独立安全功能或任何安全标准符合 | 用一个公式给出所有故障的停止时间，并在说明书写“符合功能安全” | `MA-026` 停止链报告 |
| C-STOP-009 | 停止链故障矩阵至少覆盖：运动中 Flash 擦写、控制任务死锁、控制心跳正常时单独挂起 Kernel、panic/复位、欠压与 Wi-Fi 高负载，并在每种状态下触发急停；使能脚用逻辑分析仪记录，实际停止用编码器/IMU 或高速视频记录 | 只在空闲状态测一次急停就给出停止上限 | `MA-026` |
| C-STOP-010 | 物理结果未知时，按能力的恢复属性重新测量、补偿或交给人工处理，不盲目重复；恢复后不重放旧运动命令（C-OUT-001） | 转向结果未知，重启后系统再转一次“确保到位” | E2 总通过标准；`MA-026` |
| C-STOP-011 | 续租是“状态读到 `ACTIVE` → 挑战 → 续租”的请求链，每个回答到达的工作项都重新判断是否应停止，仍允许才发出下一请求；需要停止时当项即请求停止，停止请求走独立通道，不等待在途的挑战、续租或状态读取；状态或控制请求被拒、输出无效或超时都请求停止（ADR-0052） | 撤销时一个续租挑战在途，挑战回答到达后 Core 仍发出续租，截止被推到撤销之后签发的 nonce + `L` | `ManagedExecution.revocation_while_a_renewal_challenge_is_held_sends_no_renewal`、`ManagedExecution.lost_link_ends_output_by_the_executors_own_lease`、`ManagedExecution.open_requests_wake_a_host_that_sleeps_until_the_reported_deadline`、`ExecutorMailboxTest.a_stop_lane_answers_while_other_lanes_wait`；C-PERMIT-005 |
| C-STOP-012 | 状态读取答 `NOT_FOUND` 只有在该读取发出于 Core 看到 Provider 终态之后时，才证明许可从未开始并解除隔离；更早发出的读取再读一次；执行端答 `NOT_FOUND` 后必须拒绝该许可此后再开始；许可交给 Provider 后第一次状态读取等一个轮询周期（ADR-0052） | 终态之前发出的读取在许可转发到远程闸门之前答 `NOT_FOUND`，Core 据此释放执行器，许可随后到达闸门并开始输出 | `ManagedExecution.not_found_fences_a_permit_still_on_its_way_to_the_gate`、`ManagedExecution.not_found_proves_not_started_only_from_a_read_after_the_terminal`、`ManagedExecution.a_permit_forwarded_within_a_poll_period_starts`；远程闸门待 `MA-038` |

## 本地 gate 生命周期与部署约束

`control.h` 的输入来自可信本地适配器，不能将任意网络请求直接作为已认证 owner 许可。每次创建 gate 都必须更新 executor_boot_epoch；启动无许可且无输出，首次有效样本前 check 返回 NOT_READY。同一 permit 的 sequence 严格递增，每周期同时检查租期和最后样本年龄。样本时间必须属于执行端时钟，远端到达时间不能替代采集时间。

停止分为立即禁止输出、等待本地测量、已确认三步；`ControlGate.cancellation_and_stop_measurement_are_separate_facts` 验证不将取消受理当作停止确认，未确认时 destroy 返回 BUSY。确认不能代表目标验证或 Core 资源释放。错误返回不授权输出，实际禁用及定期调用由适配器负责；线程挂起后的停止仍需要独立硬件/监督链。独立 C99 链接和无堆引用由 control_gate_from_c99_without_core、control_gate_does_not_reference_heap_allocator 检查；不作为实机部署证据。


## ABI 0.35：受管理 Mock 执行（ADR-0050）

Scene 的原始有限声明和所有者启用偏好分别保存。启用须持久且声明完全匹配才可消费；关闭立即禁止新签发/续租。管理意图 16 使用现有 MANAGEMENT 会话、非零 command_id 和精确 revision；同身份重试返回原回执，NO_CHANGE 不增加 Scene revision。容量满拒绝，旧偏好不自动淘汰。公开命令与边界见 `SceneApi.disabled_default_enable_waits_for_durability_close_blocks_immediately`、`SceneApi.retries_keep_original_receipt_and_no_change_keeps_scene_revision`、`SceneApi.conversation_session_cannot_enable_and_zero_command_is_rejected`、`SceneApi.invalid_cross_reference_does_not_consume_capacity`。声明保存格式正常往返由 `SceneApi.original_definition_record_round_trip_preserves_constraints_and_preference` 验证；跨重启/中断专项未执行。

ExecutionBinding 声明原能力、Executor、Scene 的精确版本及数值投影。参数取自调用已授权的同一规范 Scope，每个包络字段指定上下限来源字段及单位，最多 8 个。每个 Executor 的所有 Binding 共享一个规范顺序中的首个独占 ACTUATOR，避免多条独立资源链同时占用同一本地 gate。资源仍经原公平全组准入，无临时执行旁路。

许可 `permit_id` 使用持久 B3 原执行 attempt.request_id，`operation_id` 保持接受时的原动作身份。调用接受顺序可能被优先级/轮转改变，故不能用 operation_id 作为 gate 的单调许可序列。该 Executor 的独占资源使派发尝试顺序单调；所有代际仍需匹配。`ManagedExecution.priority_reordering_keeps_distinct_monotonic_dispatch_permits` 验证正常重排。

本地端口在 Kernel owner 线程有界执行，不访问私有表、不等待 I/O（0.36 起改为异步请求，见下节）；Provider 在 submit 内安装许可，工作循环随后逐样本经过 gate。停止与轮询在日志容量门控前推进；观察替换/过期、Grant 撤销、Scene 关闭、Task 取消或关闭阻止续租。`ManagedExecution.grant_revocation_stops_without_waiting_for_durable_cleanup`、`ManagedExecution.session_renews_with_fresh_nonce_and_normal_close_never_resumes_old_motion`、`ManagedExecution.procedure_expires_normally_without_renewal_and_never_revives`、`ManagedExecution.observation_replacement_stops_original_session` 覆盖正常消费。

Provider 终态在停止未确认时继续占用可靠完成槽；不会阻塞其他槽。精确停止测量先提交，资源等待其持久后才释放；终态结果与 Task 进度继续 B4 同批。观察依赖在结果应用时再次检查，过期结果以 STALE 失败保留副作用事实，正文不进入下一步。`ManagedExecution.unconfirmed_stop_retains_terminal_and_quarantine_blocks_other_agent`、`ManagedExecution.replaced_dependency_cannot_publish_completed_step_after_execution_ended`、`ManagedExecution.cancelled_task_is_not_reopened_by_late_completion` 验证这些边界。

无法追加的显式关闭只在本地停止已确认、Provider 借用结束后清理易失占用，不伪造持久完成；旧日志中的可能执行仍按 UNKNOWN 保留。该失效路径仅做代码审查，不列为恢复实验通过。目标结论首版只表示 Mock 的 UNVERIFIED/SATISFIED/UNSATISFIED；不宣称已实现 C-STOP-007 的真实传感测量、误差与容差档案。

原调用超时同样阻止控制会话续租，见 `ManagedExecution.call_deadline_stops_session_independently_of_lease_renewals`。例行轮询与业务处理轮转，`ManagedExecution.one_item_steps_share_routine_polling_with_terminal_processing` 覆盖单项 step 预算；紧急停止仍优先。原完成与主动对账先共同验证原始结果，再复核观察依赖；`CallTableTest.stale_query_preserves_effect_but_rejects_invalid_evidence_before_filtering` 覆盖错误证据不可被过滤为合法失败，并保留有效结果的副作用事实。

## ABI 0.36：异步执行端端口（ADR-0052）

`challenge`、`renew`、`stop`、`status` 都是请求：端口返回 OK 表示收下，之后恰好一次 `ayni_executor_post` 回答，可在端口内或任意非 ISR 线程投递；返回其他状态即为拒绝，Core 把它当作回答。每个执行端有控制、停止、状态三条通道，各自至多一个未回答请求，请求身份与 Provider attempt 共用一个序列。每个请求至少等待 `poll_period_us`，到期后仍未回答按结果未知处理并拒收迟到回答；未到期的请求不算待办工作，`ayni_step` 不等待执行端。投递的校验与唤醒见 C-HOST-018、C-HOST-020，`ExecutorMailboxTest.one_answer_per_request_and_late_or_duplicate_posts_are_refused` 覆盖。

派发在 `DISPATCHING` 屏障持久后发出挑战，回答到达后的工作项重新复核再安装许可（C-PERMIT-018）；续租链每一步重新判断是否应停止，撤销不等待在途请求（C-STOP-011）；`NOT_FOUND` 只从 Provider 终态之后发出的读取得到证明力，执行端答过后拒绝该许可（C-STOP-012）。停止回答不是停止证据，停止请求超时才重发（在急停轮次），被拒时由执行端自身租约收敛；只有状态回答里的停止测量能写入停止事实。

续租因此跨三次往返、四个工作项，第一次续租还要等派发挑战的往返和一个轮询周期；会话租期 `L` 的声明须覆盖这些时间与漂移余量。Provider 须在一个轮询周期内把许可转发到远程闸门，否则许可被 `NOT_FOUND` 拦下，调用以停止结束。挑战 `BUSY` 由 0.35 的稍后重试改为结束调用。Mock 执行端可立即、延后或丢弃回答，也可把许可推迟到下一次 `advance()` 才交给闸门，用于上列用例；这些只是 Mock 链路，不作为真实远程执行端或停止时限的证据。
