# ADR-0111：软停止与硬急停拆分、Core 资源级原子抢占及会话即时打断

- **状态**：已实施（阶段 2，本地验证通过，待真机 OTA 实测）；总体路线见 [低时延实时 AgentOS 架构演进计划](../architecture/low-latency-realtime-agentos-plan.md)。
- **日期**：2026-11-24
- **关联**：`ABI 0.58`（`storage_schema` 维持 `11` 不变）；C-RES-002/008、C-STOP-006、C-CALL、ADR-0110。

## 背景

阶段 1（[ADR-0110](0110-ephemeral-execution-tier-and-immediate-resource-release.md)）消除了瞬态动作间的同步 WAL 写入、内联停稳证明后的冗余 `STOP + STATUS` 往返与 Flash 隔离等待，但系统在实时中断与指令切换上仍存在四类结构性阻塞：

1. **软停止与硬急停混用**：`motor.stop` 工具与 UI 普通「取消」按钮均调用 `request_motor_stop()` 触发 `Mode::latched`，锁存硬件直至重启；无法实现“停一下后继续执行新动作”。
2. **Core 资源准入无抢占能力**：`ResourceAdmission` 禁止新请求抢占在途持有者，高优先级指令、并发自检或未来阶段 3 的传感反射规则只能排队等待前序 5–60 秒动作自然结束。
3. **电机 `begin()` 受 UI 通知背压牵连**：`MotorExecution::begin()` 将 `terminal_notice_` 作为拒单条件，UI 队列短暂满载时会误拒紧随其后的合法动作。
4. **会话单回合忙锁与取消落盘阻塞**：`wifi::Runtime::send()` 与 `Chat::receive()` 在当前回合运行中拒收新指令；`ConversationSession::cancel_turn()` 与 `discard()` 在 `Persistence::background + UnfinishedTurn::discard` 模式下仍等待 `commit_seq <= durable_seq` 并向 Flash 写入空的 `conversation_root` 来清除从未落盘的易失草稿。

## 决策

1. **ABI 0.58 与零存储变更**：
   - `AYNI_ABI_MINOR` 升至 `58`，`AYNI_STORAGE_SCHEMA_CURRENT` 保持 `11`（无存储格式变更）。
   - 新增公开原因码 `AYNI_REASON_PREEMPTED = 83`（标明在途 `EPHEMERAL` Call 因合法抢占被取消）。
   - 新增 `ayni_call_preemption_t`：`0 = AYNI_CALL_PREEMPT_NONE`（默认 FIFO，保持既有行为）、`1 = AYNI_CALL_PREEMPT_LOWER`（仅抢占严格更低优先级持有者）、`2 = AYNI_CALL_PREEMPT_SUPERSEDE`（可抢占同级或更低优先级的更早持有者）。复用 `ayni_call_request_v1` 原保留字段 `mode_reserved` 为 `preemption`（不增大结构体体积），禁止对 `AYNI_CALL_MODE_MANAGED` 或订阅请求声明非零抢占。
2. **Core 资源级原子抢占边界与 Fail-Closed 安全不变量**：
   - **先预校验、后抢占**：新 Call 必须完整通过 Grant、Scope、Scene、Observation、Schema、ExecutionBinding 与队列容量检查并入队后，方允许触发抢占；无权或越界请求绝不打断在途动作。
   - **仅限 `EPHEMERAL` 且可取消持有者**：绝不自动抢占 Tier 2 `DURABLE` 或 `AYNI_CALL_CANCEL_NONE` 持有者。多资源组请求采用全组原子判定：任一冲突持有者不可抢占，或任一目标资源/冲突域处于 `OFFLINE / FAULTED / QUARANTINED`，均放弃抢占并转入常规等待。
   - **同 `step` 接力与停稳失败隔离**：合格在途持有者收到 RAM-only `CallCancelCause::preempted`、`provider->cancel` 与紧急通道 `AYNI_EXECUTOR_STOP`；确认内联 `AYNI_CONTROL_STOPPED` 证明后资源即刻在 RAM 置 `FREE`，同一次 `ayni_step()` 即可将资源授予抢占等待者；若停止失败或缺乏合法证明，资源保持 `QUARANTINED` 回落独立 `STOP + STATUS` 轮询，新 Call 绝不越过隔离态启动。
3. **`ConversationSession` 后台丢弃零 Flash 取消**：
   - 仅当 `Persistence::background && UnfinishedTurn::discard` 时：`cancel_turn()` 不再等待 `goal.commit_seq <= durable_seq` 即刻向 Core 提交 `AYNI_INTENT_GOAL_CANCEL`；`turn()` 观察到已取消终态即刻进入 `discard()`；当草稿从未写入磁盘根文件（`!draft_persisted_`）时，`discard()` 在 RAM 直接清空草稿并释放未确认正文，跳过 `Phase::save` 直接回到 `Phase::ready`（0 Flash 写）。`durable` / `retain` / `retain_and_append` 行为保持 100% 不变。
4. **软停止与硬急停拆分及 ESP32 会话打断（Barge-In）**：
   - `motor.stop` 工具、Core `AYNI_EXECUTOR_STOP` / `cancel()`、UI 普通取消与 Barge-In 均执行可恢复 **`soft_stop`**（停 PWM + 读回低电平 + `ayni_control_confirm_stopped()` + 保持 `Mode::armed`，仅当硬件停稳失败时升级 `Mode::latched`）。`motor.stop` 等待真实读回确认后才返回 `stop_confirmed: true, armed: true`。
   - **触屏红色独立急停按钮**、电源关闭与硬件/联锁故障保持锁存 **`hard_estop`**（`Mode::latched`，递增 `safety_epoch`，需重启恢复）。
   - `MotorExecution::begin()` 移除 `terminal_notice_` 误拒单条件，UI 快照推送改为非阻塞尽力重试。
   - `wifi::Runtime::send()` 与 `Chat::receive()` 解除单回合忙锁，引入 1 槽最新优先（latest-wins）`pending_user_`：新对话指令到达时立即中止旧 HTTP/TTS（`api_.cancel_all()`）、立即触发电机 `soft_stop`（不等 LLM 思考）、取消旧 `CANCEL_ON_REBOOT` Goal 并在零 Flash 丢弃后立即启动新回合。

## 后果与备选方案

- **后果**：软停止与硬急停职责清晰，正常取消或语音/文字打断不再锁死电机；Core 具备面向多任务与阶段 3 传感反射的通用资源抢占原语，且无需升级存储 Schema；代价是 `ABI 0.57` 二进制需按 `0.58` 重新编译，且被打断的未完成回合草稿在 `background + discard` 模式下不写入持久历史。
- **备选方案**：曾考虑仅做 Goal 级替换而不做资源级抢占，或将软停独立为新工具 `motor.soft_stop`。前者无法支撑阶段 3 传感反射与多 Goal 并发抢占，后者不符合自然语言/大模型调用 `motor.stop` 的直觉。

## 验证

- **主机 ASan/UBSan 便携单测**：`ayni_portable_unit_tests`、`ayni_strategies_tests`、`ayni_model_protocol_tests`、`ayni_c_abi_smoke`、`ayni_control_c_abi_smoke` 全部通过（`ctest` 1,736/1,736，含 `Preemption.*` 10 项：同级 `SUPERSEDE`、跨级 `LOWER`、保护 `DURABLE` 与 `CANCEL_NONE`、权限/Scene/Schema 预校验不变量、停稳失败隔离 `QUARANTINED`、多资源组全或无、非法参数拒绝、`ConversationSession` 零 Flash 取消与共享正文保护、`retain` 回归，以及 `AYNI_GOAL_CONTEXT_AGENT + EPHEMERAL` 上下文擦除与回放安全修复）。
- **ESP32 Wi-Fi 主机与集成测试**：`ayni_wifi_checks` 及集成套件 100/100 通过（含 `MotorCheck` 验证 `preemption == AYNI_CALL_PREEMPT_SUPERSEDE`）。
- **ESP-IDF 6.0.3 固件编译**：ESP32-S3 Xtensa `-Werror` 编译通过（`ayni_wifi_chat.bin` 2,817,440 B，10% 分区余量，`sdkconfig` 未修改）。
- **静态与可读性检查**：`clang-format 18.1.8`、`tools/check_deps.py`、`tools/check_contracts.py`、`tools/check_readability.py`（0 新增违规）、`git diff --check` 全部通过。
- **尚未执行**：ESP32-S3 实机 OTA 烧录与板端打断/软停实测按用户要求暂缓。
