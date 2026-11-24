# Ayni 低时延、安全、实时 AgentOS 架构演进计划

> **文档性质**：Ayni 核心架构演进计划与分阶段验收目标；各阶段能力在附有代码实现与对应验收证据前均属于规划中契约。\
> **日期**：2026-11-24\
> **关联**：[ayni_v1.0.md](../ayni_v1.0.md)、[foundation-and-experiment-plan.md](foundation-and-experiment-plan.md)、[ADR-0110](../adr/0110-ephemeral-execution-tier-and-immediate-resource-release.md)

## 1. 背景与根因

基于 ESP32-S3 真机实测（`da571d2` 日志）与 `core/` 执行链路审查，现有 Ayni 核心存在将「实时物理/瞬态控制」等同于「可崩溃重放持久化事务账本」的系统性错配：

1. **动作结束仍被 Flash `fsync` 锁死在隔离态**：执行器完成动作并确认引脚拉低停稳后，Core 仍强制补发 `STOP + STATUS` 往返，并将资源保留在 `quarantine`（`Stage::retiring`），直到 `EXECUTION_STOPPED` 与 `CALL_COMPLETED` 写入 Flash 并推进 `durable_seq` 后才解除隔离。
2. **每步工具决策与派发受同步 WAL 门控**：`admission_ready()` 等待 `task.commit_seq <= durable_seq`，`progress_dispatch()` 等待 `DISPATCHING` 落盘，导致无跨重启重放风险的瞬态动作每次交接消耗 ~480–510 ms。
3. **中间结果与提案碎片落盘**：`apply_delivery()` 对每个完成工具结果和模型提案调用 `FileBodies::confirm()`，在多动作回合内制造大量小文件写入积压（实测 55 次写入 / 7.68 s）。
4. **缺少可恢复软停止、原子抢占与传感实时闭环通道**：现有 `motor.stop` 仅提供锁存急停，打断在途动作需要完整取消+落盘+重建流程；高频传感器尚无绕开 LLM 与 Flash 的本地反射通路。

## 2. 旧安全契约重审原则

从第一性原理区分**「防崩溃重复派发（数据库 WAL 语义）」**与**「实时物理执行安全（控制安全语义）」**：

- 派发前同步 Flash 屏障（`DISPATCHING`）仅保护可能跨重启恢复重放的不可幂等外部事务（如支付、邮件、持久闹钟）。
- 对于**重启永不重放**的实时物理动作与瞬态工具，物理安全由**内存授权校验 + 本地执行端包络限幅/联锁 + 看门狗/租约 + 独立急停链**保证，而非由 Flash `fsync` 保证。

| 维度 | 旧契约 | 新定位 |
|---|---|---|
| 静态授权、包络限幅、租约看门狗、独立急停锁存 | RAM + 执行端控制门 + 硬件停止链 | **完全保留并强化**，零 Flash 依赖 |
| 执行器隔离（Quarantine） | 每次完成均先隔离直至 `STOP + STATUS + Flash` | **仅停止未确认/超时/故障时隔离**；确认物理停稳后立即在 RAM 置 `FREE` |
| 派发前 Flash 屏障（`DISPATCHING`） | 所有 Call 强制同步落盘 | **按能力分级**：仅 Tier 2 持久事务启用；Tier 0 零同步屏障 |
| 回合中间步骤与正文持久化 | 每步写 WAL 与 `FileBodies` | 瞬态回合中间纯 RAM；仅回合边界/持久边界落盘，审计异步化 |
| 停止语义 | 仅有锁存急停与慢速异步取消 | 明确拆分**软停止/抢占（保持 Armed）**与**硬急停（锁存需人工复位）** |
| 执行器会话粒度 | 每次动作独立申请、握手、隔离、回收 | 单次瞬态调用 + 长驻 `ControlSession`（一次握手、会话内流式下发） |

## 3. 三平面解耦架构

按时间尺度拆分为三个独立平面，慢平面（Flash / 网络 / LLM）永不阻塞快平面（授权判定 / 资源切换 / 抢占 / 传感反射）：

1. **平面 1：实时控制与硬安全平面（微秒–5 ms，零 Flash I/O）**
   - `ControlGate`（包络限幅、单调时钟租约、代际校验、看门狗，只能收紧不能扩权）。
   - `soft_stop`（停输出 + 读回确认停稳 + 保持 `ARMED`，`< 1–2 ms`）与 `hard_estop`（硬件失能锁存 + 递增 `safety_epoch`）。
2. **平面 2：内存内核协调平面（Ayni Fast Core，亚毫秒–2 ms，零同步 Flash 阻塞）**
   - 内存权威 Grant/Scope/Scene 表、三态资源表（`FREE / OCCUPIED / QUARANTINED`）。
   - 三级执行语义：
     - **Tier 0（`EPHEMERAL`）**：实时物理与瞬态工具，限定于 `CANCEL_ON_REBOOT` 目标，运行期零 WAL、零 `durable_seq` 门控、停稳即刻 RAM 释放资源。
     - **Tier 1（`SESSION_CHECKPOINT`）**：普通会话回合，仅在回合边界持久化历史与最终结果。
     - **Tier 2（`DURABLE`）**：不可幂等外部持久事务，完整保留现有 `PREPARED/DISPATCHING` WAL 屏障与 `RESULT_UNKNOWN` 对账。
   - 原子抢占原语（`PREEMPT / SUPERSEDE`）：预校验新请求 -> 调 `soft_stop` -> 确认停稳 -> 同一 step 直接切换执行。
   - 确定性本地传感反射引擎：`ObservationSlot`（无锁最新值）-> `ReflexRule`（纯确定性本地规则直接触发抢占，异步上报低频事件给 LLM）。
3. **平面 3：慢速认知与持久化平面（百毫秒–秒级，完全异步）**
   - LLM 推理、多步规划、回合级粗粒度持久化、异步定长审计环 `AuditRing`、OTA 与管理接口。

## 4. 四阶段实施与验收路线

| 阶段 | 核心内容 | 目标指标（Ayni 内部开销，不含云端模型） | 关联 ADR |
|---|---|---|---|
| **阶段 1（进行中）** | `ABI 0.57` + `storage_schema 11`（不兼容旧卷）；`AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT` 重启自动取消定理；Tier 0 `AYNI_CALL_DURABILITY_EPHEMERAL` 回合中间零 WAL / 零中间 `FileBodies` 落盘；本机可信执行器 `AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF`；内存即时释放隔离；ESP32 电机线程队列阻塞即时唤醒 | 动作间交接 `append=0`，`between/before/after` 从 ~480–510 ms 降至 `< 10–30 ms`；`challenge -> pwm-started` `< 2 ms`；重启零重放 | [ADR-0110](../adr/0110-ephemeral-execution-tier-and-immediate-resource-release.md) |
| **阶段 2（规划）** | 拆分 `soft_stop`（保持 Armed）与 `hard_estop`（锁存）；Core `PREEMPT / SUPERSEDE` 原子抢占原语；解除 `Chat` 单回合串行锁，允许新用户指令即时打断在途动作 | 打断并切换新动作 `5–10 ms`；停止失败自动转 `hard_estop + QUARANTINED` | 待定 |
| **阶段 3（规划）** | 扶正 `ControlSession` 为连续运动主路径（一次握手、包络内流式换段、后台续租）；新增无锁 `ObservationSlot` 与本地确定性 `ReflexRule` 反应通道 | 连续动作段间 `< 2 ms`；传感器避障/反馈闭环 `< 5–10 ms` | 待定 |
| **阶段 4（规划）** | 对话历史回合级粗粒度合并落盘 + 异步定长审计环 `AuditRing` | 发布写盘 `< 200–300 ms` 且异步不阻塞下一轮输入 | 待定 |
