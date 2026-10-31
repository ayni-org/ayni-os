# ADR-0063：Task 订阅、代际租期与可靠控制

> **现行目标补充（2026-10-30）**：安全关键分支目标已由 [ADR-0088](0088-safety-branch-coverage-target.md) 统一为 ≥80%，原90%目标弃用。以下保留本 ADR 当时的决定、测量及验证事实。

- **状态**：已接受（正常实现、本地验证与独立审查见 #84）
- **日期**：2026-10-27
- **关联**：P4 / MA-008B；ADR-0044、0045、0050、0052、0062；C-SUB-001～005

## 背景

P4 日历和任意线程 Host 观测/Mock 媒体已分别交付为 #82/#83。连续订阅必须复用调用的授权、资源、持久派发屏障和完成所有权，同时避免把两个 Task 的同源样本混为一个事实。Task 归属、共享通过资源容量以及 M1 Mock 媒体范围已由用户确认（2026-10-27）。

## 决策

1. **用户选择。** 默认 30 秒、半租期由 Kernel 自动复核权限和运行限制后续租；暂停/取消/关闭时停止续租并请求关闭。正常重启及暂停恢复后原 Task 复核权限/资源、以新代际继续；ACTIVE 后订阅后台保留、Task 可继续分析/模型步骤。（产品语义，用户选定推荐项，2026-10-27。）新故障/恢复专项继续暂停；未知结果不据此自动重试。
2. **调用归属。** 复用 CallTable、CallRuntime、ProviderMailbox、ResourceAdmission，不建立平行 SubscriptionTable。CallContract 的可选订阅契约绑定静态 source/version 和有限 lease；CallState 的 SubscriptionFact 拥有 phase、desired、foreground/background attachment、代际和已确认控制序号。租期的 monotonic 截止归 boot-local CollectionLease，独立于较短的 transport Delivery；UNKNOWN 终态归还 borrow 后仍保留资源至已确认/待确认请求截止的最大值。
3. **后台引用。** 首次 STARTED 应用、Task 前台 call 释放及 attachment 变化在同一 transition。之后 renew/close 不要求 Task.current_call 仍指向它，也不能结束另一条当前调用。Task 的所有未结束订阅仍阻止其安全回收。每个 transition 仍至多一条 Call 行；Task/authority 事实先立即关闭所有相关 gate，再逐 Call 持久化停止状态，不放大批次预算。
4. **每代派发。** 原 logical Call/step 身份保持；每代都有独立 operation、attempt、durable dispatch barrier 和 ActionRecord 一次 claim。初代 collection generation 使用原 operation ID，重开额外分配 Kernel ID，同时作为新代 operation。generation 在整个 Kernel 内不复用，媒体的 source/boot/generation 回收不会清理另一订阅。仅旧 generation 已确认停止且 borrow 归还后重开；RESULT_UNKNOWN 保留并等待显式对账，不作自动重发。C-ID 中 operation/logical-call 的一一对应规则对订阅扩展为一个逻辑订阅的多代 operation。
5. **可靠控制。** STARTED（控制序号 1）及每次 RENEWED 使用 owner 提前预留的独立 metadata lane，和取消/终态并存、不可被进度覆盖。ACK 必须对应原 opening、期望 kind/序号。Owner 保存请求的绝对 deadline，ACK 只确认该值，不从到达时间重新加租期；旧租期过期后迟到 ACK 不复活 EXPIRED。所有 consume/drain/closing 路径共用明确的消息分类，ACK 不是 terminal。
6. **停止与资源。** 暂停、Task 终态、授权/来源失效、lease 到期先关闭 producer gate，同时使已缓存 scoped 样本及旧 ObservationRef 不可用；在物理派发/续租前完成此拒绝前沿，即使 journal 暂无容量。新采集不再续租，Provider 收到非阻塞 close；Provider 自身还必须按有限截止停止。过期或取消请求本身都不是 borrow 返回；终态或合法 NOT_RETAINED 才能复用对应 I/O 身份。已知停止可释放资源，UNKNOWN 结束 borrow 后仍保留至最长已请求租期；owner 查询也等待该隔离结束。
7. **样本隔离。** ObservationTable 保留 Host 每源槽，另按 Call 容量预留 Task-scoped 槽；ObservationRef 追加 subscription_id。Scene 前提按 Task 查源，current 校验完整 scoped 身份和 active lease。两个 Task 同源并行，各自独立权限/租期/序号；同 Task 同源仍 desiredOpen 的第二订阅在准入时拒绝，正常重开复用原 Call。显式关闭后可以用新 step，最新 generation 选中 scoped 事实，不依赖槽顺序。关闭只保留诊断，不因 TTL 尚未用尽而继续支持动作。
8. **端口与媒体。** 每 Call 有稳定 opaque sink、锁下 opening/gate 投影与独立 gap 槽，复用 #83 FIFO；生产者不访问 Task/Grant。Sink、样本和 media 必须使用同一 collection generation，不能自报另一有效代际。Mock 在关闭/过期时回收该代字节；Core 始终没有原始媒体。捕获、读取、外发和保留仍分别授权。
9. **等待。** 用户确认分析后等待新样本/订阅状态、忙时只用最新样本（2026-10-27）。新增 WAIT_SUBSCRIPTION 唯一策略意图和 Task WAITING 生命周期；TaskTable 持久保存 generation/sequence/phase 前沿，CallRuntime 在 owner 消费新样本或状态变化后有界唤醒一次，不轮询，不因相同状态续租确认唤醒。暂停保留等待，恢复/正常重启遇新代际唤醒；终态清除。Task 进入等待前预留 wait/wake/final-cancel 三次 revision，其他管理操作不能花掉 wake+final-cancel 余量。
10. **兼容。** ABI 0.43，所有 0.x 消费者重编译，schema 2 保持。Subscription REQUIRED Call v6/v7 含契约和事实（v7 擦除）；含 scoped ObservationRef 的物理 Call 用 v8/v9（v9 擦除），原 v3/v5 仍可读。Task REQUIRED state v13～16 对应普通/授权/重新授权/提高上限，固定84字节，仅有等待前沿时使用。所有 codec/layout/预算和 journal/snapshot 配对随实现同步；旧 reader 明确拒绝新 REQUIRED 版本并保留目录。

## 后果

Task 可以同时持有观察订阅并执行有限分析；跨记录一致性比一条前台调用更复杂，需要显式审查 journal/snapshot、Task 回收与迟到控制。共享资源容量不共享 Task 私有观察事实。唯一代际 ID 消耗有限 Kernel ID 空间，耗尽必须拒绝，不能回绕。现有观测源/Provider/Call 契约的 ABI 解码随改动移至 kernel/api（F10），typed 校验仍归 catalog。

## 备选方案

- 独立订阅管理器和第二套授权/资源/观察表：重复机制并分裂不变量。
- ACTIVE 一直占 Task.current_call：持续观察期间无法使用同一 Task 分析，用户未选此项。
- 每订阅 generation 都从 1 开始而媒体仅按 source/generation 回收：两个同源订阅相互清理。
- 每次 ACK 重新加租期或租期到期即释放 borrow：迟到结果扩租或复用仍被生产者借用的槽。

## 验证

Task 所有订阅、可靠开始/续租、暂停/撤权关门、跨 Task 媒体隔离、新代际重开及 WAITING 的正常用例已在 Native 通过，公共头/C99 布局已构建，独立只读审查发现已处理；完整命令、数量、内存与 CI 结果集中在 [#84](https://github.com/RobotSe7en/ayni/pull/84)。正常 orderly reopen 只作为本轮正常功能证据，不声明故障恢复竞态、ESP32、真实媒体或完整 E1–E4 已验收。

P4 合并后统一量测见 [#85](https://github.com/RobotSe7en/ayni/pull/85)：Core 行覆盖达到通用 75% 目标；保守安全范围取完整 Core、permit gate、Mock/Mock execution 及其运行头文件，安全分支仍未达到通用 90% 目标，作为明确的质量偏差保留。此次范围比历史窄口径更广，不直接横比；不修改目标或删除防御分支。按用户现行政策，仅运行既有测试，不为追数重启暂停的故障/恢复或 mutation 专项，当前正常功能交付不作为安全/Core Alpha 验收。分子分母、聚合方法、复现命令与未入库原始数据的边界集中在 #85。
