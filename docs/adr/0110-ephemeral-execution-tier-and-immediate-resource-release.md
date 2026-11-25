# ADR-0110：瞬态执行等级（Tier 0）、重启自动取消与内存即时释放资源

- **状态**：已实施（阶段 1，主机测试与 ESP32 固件编译已通过，待真机 OTA 实测）；总体路线见 [低时延实时 AgentOS 架构演进计划](../architecture/low-latency-realtime-agentos-plan.md)。
- **日期**：2026-11-24
- **关联**：`ABI 0.57`、`storage_schema 11`；C-STORE-004/010、C-CALL、C-RES-008、C-COMMIT；计划 §2.1、§2.8、§2.10。

## 背景

ESP32-S3 实测日志（`da571d2`）显示，即使批处理优化合并了部分追加，单次电机动作前后交接仍耗时 478–508 ms，且执行器在物理停稳后仍被 `STOP + STATUS` 往返与 `Stage::retiring` 等待 Flash `durable_seq` 锁定隔离；同时 `admission_ready()` 与 `progress_dispatch()` 的同步 `durable_seq` 门控以及 `apply_delivery()` 对每步结果触发 `FileBodies::confirm()` 导致中间写入积压 55 个文件（7.68 s）。

这些同步落盘屏障原本用于防止不可幂等外部事务在断电重启后被恢复重放。对于重启后绝不重放的实时物理动作和瞬态会话，强制同步 WAL 屏障不增加物理安全，反而成为核心高延迟阻塞点。用户确认 Ayni 尚未发布，允许不兼容旧存储历史，优先按干净自描述契约推进阶段 1。

## 决策

1. **版本升级与干净不兼容边界**：公开 `AYNI_ABI_MINOR` 升至 `57`，`AYNI_STORAGE_SCHEMA_CURRENT` 升至 `11`。无旧 `schema 10` 迁移代码；旧卷按 `C-STORE-004` / `C-STORE-010` 在新写入前明确拒收（`AYNI_RESTRICTION_LOG_UNSUPPORTED`）并保留原数据，由操作方显式重建。
2. **显式持久化目标恢复策略**：`ayni_work_profile_v1` 新增 `ayni_goal_recovery_t recovery`（`0 = AYNI_GOAL_RECOVERY_RESUME` 默认保持原可恢复语义；`1 = AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT`），直接持久化进 `schema 11` 的 `GOAL` 记录。重启进入新 `boot_epoch` 后，`admission_ready()` 立即拒绝对旧 boot 的 `CANCEL_ON_REBOOT` Task 执行 `decide()`，`CallRuntime` 拒绝重派其旧 Call，`do_due_work()` 自动将该类旧 boot 活 Goal/Task 置为 `CANCELLED`（`AYNI_REASON_BOOT_RESET = 82`）。
3. **能力持久执行等级（Tier 0 vs Tier 2）**：`ayni_call_contract_v1` 新增 `ayni_call_durability_t durability`（默认 `0 = AYNI_CALL_DURABILITY_DURABLE` 保持既有 Tier 2 WAL 屏障与 `RESULT_UNKNOWN` 对账；`1 = AYNI_CALL_DURABILITY_EPHEMERAL`）。阶段 1 禁止对订阅或远程查询对账能力声明 `EPHEMERAL`。Core 在准入阶段硬性要求：**仅 `CANCEL_ON_REBOOT` 目标的 Task 允许调用 `EPHEMERAL` 能力**，普通 `RESUME` 目标调用一律返回 `AYNI_DENIED` / `AYNI_REASON_INTENT_UNSUPPORTED`。
4. **瞬态回合零 WAL 易失叠加层与快照一致规则**：
   - `Task` 新增仅驻留 RAM 的 `TaskVolatile` 叠加层（记录在途 `EPHEMERAL` Call ID、易失代数、易失步骤计数、易失模型计数与易失检查点元数据；易失步骤从槽位末端逆序增长，避免与持久步骤碰撞），`ayni_get_task()` 投影实时视图。
   - `CANCEL_ON_REBOOT` Task 执行 `EPHEMERAL` Call 与瞬态进度时，不调用 `work.transitions.next()`、不写 WAL、不推进 `commit_seq`、不触发 `FileBodies::confirm()`、不等待 `durable_seq`；`call_store` 在日志与快照中均跳过 `ephemeral` 行；`EPHEMERAL` Call 不允许 `AYNI_CALL_MODE_MANAGED`。
   - 实时 Grant/Scope/Scene/Observation/Export 校验与撤销中止 100% 保留；遭遇持久边界（`COMPLETED`/`FAILED`/`hold`/`WRITE_MEMORY`/Tier 2 Call/用户 `CANCEL`）时，将累积易失模型计数与最新检查点合并刷入 WAL；`EPHEMERAL` Call 遇 `ABANDONED` 时持久化普通 `AYNI_RESULT_UNKNOWN` 挂起（不置位 `task.call_unknown`，保证重启回放自洽）。
   - 仅当 `task.checkpoint_seq == 0` 时检查点保持纯 RAM 易失态；若混合 Tier 0 + Tier 2 任务已有持久 checkpoint，仅在检查点内容实际变更时随过渡持久化，保证后台压缩快照始终与 `checkpoint_seq` 一致且无需额外分配第二份检查点缓冲或推迟压缩。
5. **本机可信执行器内联停稳证明与内存立即释放隔离**：
   - `ayni_executor_v1` 新增 `uint32_t flags`（`AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF = 0x1u`，默认 0）；`ayni_provider_result_v1` 新增可选 `const ayni_executor_status_v1 *execution_status`，在 `ayni_provider_post()` 中按值拷贝。
   - 仅当关联执行器显式注册了 `INLINE_STOP_PROOF`、且内联状态通过与 `PermitTable::observe()` 相同的严格校验（`permit_id`、`operation_id`、`AYNI_CONTROL_STOPPED`、`stop_cause`、`target` 全部合法）时，Core 直接确认物理停稳，跳过冗余 `STOP + STATUS` 往返；否则强制回落原有独立 `STOP + STATUS` 并保持隔离。
   - 对 `EPHEMERAL` 物理 Call，一旦 `lease->stopped` 已确认，资源在 RAM 内立即解除 `quarantine` 回到 `FREE`，不等待 Flash `durable_seq`；停止未确认、超时或故障仍保持 `QUARANTINED`。
6. **ESP32 适配与电机线程即时唤醒**：`wifi.chat`、`probe` 与 `MotorCheck` 目标启用 `CANCEL_ON_REBOOT`；`wifi.model.tools`、`motor.run`、`motor.status`、`motor.plan`、`motor.stop`、`screen.show_text`、`speaker.play_tone` 声明 `EPHEMERAL`，`wifi.model.reply` 与 `speaker.speak_text` 保留 `DURABLE`。`MotorExecution` 启用 `INLINE_STOP_PROOF` 并在 `stop_output()` 确认停稳后随回执上报门控快照；`MotorExecution::run()` 改为 `xQueueReceive(inbox_, &message, 1)` 队列阻塞即时唤醒，消除每步 10 ms 级 `vTaskDelay(1)` 轮询延迟。

## 后果与备选方案

- **后果**：`CANCEL_ON_REBOOT` 瞬态多动作回合的动作间 WAL 写入降至 0，资源在确认停稳后同 step 解锁，Tier 2 持久化能力与既有契约零削弱；代价是 `schema 10` 旧卷不可直接升级，须清空重建。
- **备选方案**：曾评估“保持 `schema 10` 仅靠目录声明重启取消”及“仅去掉 `durable_seq` 等待但仍每步异步写 WAL”。前者在无目录旧二进制读取新卷时不够自描述；后者在高频动作/传感场景下仍会写满 `JournalRing` 并增加 Flash 磨损，均不如本方案干净彻底。

## 验证

- **便携 Core + 策略 + C99 ABI 合约回归（macOS arm64 AppleClang, ASan/UBSan）**：1,724/1,724 全部通过，包含新增 `tests/unit/ephemeral_execution_test.cpp` 7 项测试（验证 schema 11 拒收 schema 10 卷、EPHEMERAL 准入约束、多动作零中间 WAL 回合、`max_turns` 计数折叠、伪造/未授权内联证明 fail-closed 隔离回落、断电重启自动取消且零重放、在途 Grant 撤销 / Scene 关闭即时停机，以及混合 Tier 0 + Tier 2 持久回放）。
- **ESP32 Wi-Fi 主机侧检查（macOS）**：100/100 全部通过。
- **ESP-IDF 6.0.3 Xtensa GCC `-Werror` 固件编译**：通过（`ayni_wifi_chat.bin` 0x2ae910 字节，分区剩余 11%）。
- **静态与规范检查**：`clang-format 18.1.8`、`tools/check_deps.py`、`tools/check_contracts.py`、`tools/check_readability.py`（无新增违规）及 `git diff --check` 全部通过。
- **未执行项**：Linux `hosts/native` 与 `providers/shell` 单元测试依赖 Linux 平台 CI；ESP32-S3 实机 OTA、旧 schema 10 卷重置与动作间隔实测待用户确认后执行。
