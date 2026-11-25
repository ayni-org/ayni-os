# ADR-0112：流式控制会话（`ControlSession`）、三缓冲无锁传感槽（`ObservationSlot`）与本地确定性反射引擎（`ReflexRule`）

- **状态**：已实施（阶段 3，便携测试与 ESP-IDF 编译已验证；待实机 OTA 与板端验证）；总体路线见 [低时延实时 AgentOS 架构演进计划](../architecture/low-latency-realtime-agentos-plan.md)。
- **日期**：2026-11-24
- **关联**：`ABI 0.59`（`storage_schema` 维持 `11` 不变）；ADR-0050/0052/0062/0084、ADR-0110、ADR-0111。

## 背景

阶段 1（[ADR-0110](0110-ephemeral-execution-tier-and-immediate-resource-release.md)）与阶段 2（[ADR-0111](0111-soft-stop-preemption-and-barge-in.md)）分别消除了瞬态动作间的同步 WAL 写盘阻塞，并补齐了可恢复软停、Core 资源级原子抢占与实时打断，但在两类实时闭环场景下仍有结构性瓶颈：

1. **多段连续运动仍按单段 `AYNI_FORM_PROCEDURE` 拆单执行**：相邻动作段之间必须停输出、读回低电平、回传策略层重新决策并重握手 `CHALLENGE` 与重建 PWM 通道，无法做到 `< 2 ms` 段间无缝切换。
2. **高频传感器缺乏无锁最新值通道与绕开 LLM 的本地确定性反应通道**：现有 `ayni_post_observation()` 走互斥锁 + 有界 FIFO（满队返回 `AYNI_BUSY` 拒绝最新样本），且消费位于 `ayni_step()` 末尾（晚于门控刷新与 Call 派发），无法满足避障、碰撞开关与关键传感器断流 `< 5–10 ms` 的实时闭环反应需求。

## 决策

1. **ABI 0.59 与零存储变更**：
   - `AYNI_ABI_MINOR` 升至 `59`，`AYNI_STORAGE_SCHEMA_CURRENT` 维持 `11` 不变。
   - 新增公开原因码 `AYNI_REASON_REFLEX = 84`（标明在途 `EPHEMERAL` Call 因本地 `ReflexRule` 被软停或抢占）。
   - `ayni_config_v1` 尾部追加 `reflex_capacity` 与 `reflex_event_capacity`（缺省 0，禁用时零 arena 占用）。
   - 新增独立 C99 头文件 `include/ayni/control_session.h`（不依赖 `core/`）与 `include/ayni/reflex.h`，并在 `include/ayni/observations.h` 新增 `ayni_publish_observation_latest()` 与 `ayni_get_observation_slots()`。
2. **`control/` 通用 `ControlSession` 与 Core `AYNI_FORM_SESSION` 多行紧致包络**：
   - 在 `control/permit_gate` 内新增零堆分配 `ayni_control_session_t`：封装 `AYNI_FORM_SESSION` 许可的开启、最多 8 段有序轨迹与实时设定点替换、逐拍 `ayni_control_sample()` 校验、段间无缝切换（不插入停机与重握手）以及 `ayni_control_renew()` 短租约后台续租。
   - 保持 ADR-0084 对 `AYNI_FORM_PROCEDURE` 禁止 `AYNI_PROJECTION_LIST_ITEM` 的约束不变；仅对 `AYNI_FORM_SESSION`，允许字段投影经由列表项，由 Core 逐项通过 Grant 与 Scene 校验后，对全部列表项求严格闭区间并集 `[min_i, max_i]` 作为 `ControlGate` 许可包络（单一实现 `catalog::execution_envelope_for_value()`）。
3. **每源三缓冲（Triple-Buffer）无锁 `ObservationSlot` 与 `ayni_step()` 消费前移**：
   - `ObservationSlots` 在 Core arena 为每个注册源分配 3 个静态槽位和 1 个原子状态字（`latest | reading | writing | dirty`），生产者与单所有者读取绝对分槽，100% 满足 C++17 内存模型与 TSan 无数据竞争要求，永不返回 `AYNI_BUSY`；仅在 `dirty` 由 `0 -> 1` 时唤醒 owner，避免高频唤醒风暴。
   - `ayni_step()` 将 `drain_observation_slots()` 与 `evaluate_reflexes()` 前移至 `apply_observation_gaps()` 之后、`calls.refresh_live_gates()` 之前，确保紧急采样在**同一 `ayni_step()`** 内立即完成门控刷新、反射触发与紧急停止。
4. **Core 确定性本地反射引擎 `ReflexRule`**：
   - `CONFIGURING` 阶段注册静态规则表，运行期通过 `ayni_set_reflex_enabled()` 纯内存启停（并支持绑定 Scene 门控）；所有条件源必须为 `AYNI_OBSERVATION_MEASURED`，拒绝 `INFERRED` 触发安全反射；内置 `cooldown_us` 冷却、`EDGE_TRIGGERED` 边沿触发与 `STALE_OR_GAP` 关键传感器断流 fail-safe。
   - 支持两类零 LLM / 零 WAL 确定性动作：
     - `AYNI_REFLEX_SOFT_STOP_RESOURCE`：同 `step` 软停并抢占指定资源上可取消的在途 `EPHEMERAL` Call（绝不破坏 `DURABLE` 或 `CANCEL_NONE`，停稳失败保持 `QUARANTINED`）。
     - `AYNI_REFLEX_PREEMPT_CALL`：对指定 Agent 当前活跃实例完成完整运行期 Grant + Scope + Scene + ExecutionBinding 预校验后，在 RAM 中直接提交 `state.ephemeral = true, state.reflex = true`（`task_id = 0`）的预编码确定性 `EPHEMERAL` Call，以 `AYNI_CALL_PREEMPT_SUPERSEDE` 原子抢占执行，终态即时回收槽位。
   - 所有触发与拒绝结果写入有界异步事件环 `ayni_reflex_event_v1`，供 Host/UI/上层模型按需异步消费。
5. **ESP32-S3 `motor.run` v3 连续换段与传感反射接入**：
   - 升级 `motor.run` 至 `v3`（`segments[1..8]` 列表参数 + `AYNI_FORM_SESSION` 2 s 短租约 + 200 ms 自动续租），`MotorOutput::transition()` 实现同方向不断流热切换与反向先断后通防直通，`MotorExecution` 实现 `AYNI_EXECUTOR_RENEW` 并在 `MotorCheck` 与系统提示词同步适配。
   - 在 `chat_config()` 启用 `observation_source_capacity = 2`、`reflex_capacity = 2`、`reflex_event_capacity = 8`，注册通用测距源 `sensor.obstacle_mm` 与受 `motor.manual-bench` Scene 门控的 `reflex.obstacle_stop` 软停规则，排空反射事件至诊断日志与状态通知，待接入实体传感器后直接挂载生产线程即可闭环。

> 后续收口：最新值顺序、并发 `AYNI_BUSY` 和单步预算的当前行为见[观测契约](../../contracts/domain/observations_and_media.md)。预算足够时同一步完成反射与停止，预算用尽时在下一步续跑。

## 后果与备选方案

- **后果**：多段连续运动无需在段间停机和重握手，高频传感器不再受 FIFO 满队丢新样或 step 尾延迟拖累，确定性安全反射可绕开大模型与 Flash 在亚毫秒至毫秒级闭环；代价是 ABI 升至 `0.59` 且 ESP32 `motor.run` 参数格式升级为 `v3` `segments[]`。
- **备选方案**：曾考虑保留旧 `motor.run` 并另增 `motor.sequence`，以及将反射启停持久化升级至 `schema 12`。按用户确认决策，直接升级 `motor.run` 避免工具冗余与大模型误选，同时保持 `storage_schema 11` 不变以减少兼容与写盘负担。

## 验证

- **便携 Core + `control/` + 策略 + C99 ABI 合约全量回归**（macOS arm64 AppleClang, Debug + ASan/UBSan）：`1,748/1,748` 通过（`ctest`，`27.02 s`），包含：
  - 新增 `ControlSessionTest.*`（5/5 通过）：覆盖多段无缝切换、硬件定时换段与超 8 段环形压缩、包络内实时目标替换与越界拒收、短租约续租与租约过期/联锁丢失 fail-closed 停机，以及 `open()` 预校验不提前武装门控与拒绝多段 `PROCEDURE`。
  - 新增 `ReflexAndSession.*`（7/7 通过）：覆盖 `AYNI_FORM_PROCEDURE` 继续拒绝列表投影（`AYNI_UNSUPPORTED`）、`AYNI_FORM_SESSION` 多行包络精确并集与短租约零 WAL 续租、400 次跨线程无锁三缓冲 `ObservationSlot` 并发写入（0 `BUSY`、0 竞争）与同 `step` 排空、`SOFT_STOP_RESOURCE` 同 `step` 零 WAL 软停 / 拒绝 `INFERRED` 源 / `DEGRADED` 与 Scene 门控 / 保护 Tier 2 `DURABLE` / 停稳失败隔离（`QUARANTINED`）、`STALE_OR_GAP` `next_deadline_us` 自动唤醒与断流 fail-safe、`PREEMPT_CALL` 零 LLM 零 WAL 抢占与槽位回收 / 边沿锁存冷却 / Grant·Scope·Agent 预校验拒收，以及 `ayni_set_reflex_enabled()` 零 WAL 切换与 `schema 11` 重启兼容。
  - `c_abi_smoke`、`control_c_abi_smoke` 与 `KernelLayout.*` 通过（固定 `ABI 0.59` 全结构体布局及 arena 布局审计）。
- **ESP32 Wi-Fi 主机与集成测试**（macOS arm64 AppleClang）：`101/101` 通过（`7.57 s`），覆盖 `motor.run` v3 6 节点 `segments[]` 规范化与 `valid_motor_trajectory()` 边界校验。
- **ESP32-S3 固件交叉编译**（ESP-IDF 6.0.3 Xtensa GCC, `-Werror`）：通过，`ayni_wifi_chat.bin` 大小 `0x2b4110` 字节（分区剩余 `0x4bef0` / `10%`），`sdkconfig` 未改动。
- **格式与静态门禁**：`clang-format 18.1.8`、`tools/check_deps.py`、`tools/check_contracts.py`、`tools/check_readability.py`（0 新增违规）、`git diff --check` 全部通过。
- **未执行项**：Linux 专属套件（`hosts/native`、`providers/shell`）待推送后由 Linux CI (`native-asan`) 验证；ESP32-S3 实机 OTA 与外接实体传感器板端验收按用户要求暂缓。
