# ADR-0062：有界观测入口、可靠失效前沿与 Mock 媒体

- **状态**：已接受
- **日期**：2026-10-27
- **关联**：P4 / MA-008B；ADR-0021、0050、0052、0061；C-OBS-001～006、C-SUB-004

## 背景

已有 ObservationTable 只接受拥有者线程发布标量。ADR-0052 要求外部观测改成任意非 ISR 线程的有界异步入口；样本队列满时仍必须保留丢帧导致的失效。M1 媒体采用引用元数据加 Mock 字节，真实媒体在 M3 接入（产品语义，用户选定推荐项，2026-10-27）。

## 决策

1. **独立交付边界。** P4 拆成日历、观测/Mock 媒体、订阅生命周期三个 PR。本文交付可信 Host 样本传输和引用，不把 Host 发布解释为订阅；STARTING/ACTIVE/续租/关闭、Task 所有和资源共享留给随后订阅 PR。现有 owner 标量入口保留。
2. **传输与归属。** API 边界一次解码 C 结构、版本和借用字符串，传输 typed 值；F10 的 source 解码也移到 kernel/api。ObservationIngress 复用 EventQueue 保存完整 metadata，ObservationTable 只拥有当前 boot 的事实、有效期和失效前沿。字节不进入 Core。队列计数与 boot 准入门由 Host ingress lock 串行；生产者不读取 owner lifecycle，不在锁内调用 wake/clock。RUNNING 开门；关闭先关门，再逐项有界锁丢弃，销毁前 runtime join 全部生产者。
3. **事件策略。** 本类样本采用 FIFO，不合并、不淘汰；满返回 BUSY 并计数。接受只说明已复制，拥有者消费时才检查采集时间、TTL、媒体 boot 和 owner、顺序及 gap。每 step 至多消费入口时已有的数量，按原工作/输入后的顺序计入 item/time 预算；控制与原输入队列互相独立。
4. **可靠失效。** 每源预留独立 gap 槽，合并为最高 `(generation, through_sequence)`，不受样本队列满影响；保留已应用前沿，使重复报告幂等。owner 每 step 在调用派发前对各源至多检查一次，把旧依赖立即变为不可用，保留旧样本作诊断。此有限源扫描属拒绝前沿维护，不算应用 item，也不随 gap 数量循环。新样本必须越过前沿；代际变化使旧引用失效，TTL/质量与 Scene 前提仍走同一个 ObservationTable 判定。显式和隐式漏号只计新增损失，不重复计数。
5. **媒体引用。** 结构含非零 ID/boot/owner/generation、源版本、用途、有限 TTL、大小和内容类型；不含指针/路径/URL。纯 schema 校验由公共 validator 共享。Mock MediaStore 在 providers/mock 内启动分配固定对象槽和字节 stride，调用时不扩容；读取复制完整对象，严格匹配全部 metadata、owner 和用途，匹配本身不代替实际读/外发授权。过期、显式关闭某源代际、新 boot 时清空回收字节；同 boot 重复初始化幂等，ID 不复用。组合根负责推进过期/生命周期回收。本 PR 不声称已自动装配订阅清理。
6. **边界与兼容。** ABI 0.42 增加 post/gap/info 和媒体头，config 追加 observation_ingress_capacity，ObservationInfo 追加 metadata/gap。主版本 0 只接受当前布局，Host/Provider/SDK 全部重编译；schema 2 与持久记录不变，引用/字节/队列不持久。无 source 时 FIFO 默认关闭；有 source 时 0 选默认 16，上限 256。Mock 默认 4 对象 × 4096 字节，每对象最多 65536 字节、最多 64 对象。边界是 M1 测试存储，不代表真实硬件缓存预算。
7. **变化场景。** 新 Host 只需使用既有 lock/wake 与 post/gap 接口；替换真实媒体存储无需修改 Core 表/调度。订阅随后在样本进知识前增加 Task/ACTIVE/boot/代际检查，复用本传输与失效机制，不引入另一张观察表。

## 后果

显式 gap 需要生产者在已知丢帧时报告；仅有下一条序号不能在它到达前推断丢失。关闭或过期回收是字节存储 owner 的责任，公共引用不自动证明字节还在。有限源扫描不证明最坏 step 时限，硬件及故障竞态仍需后续证据。

## 备选方案

- 生产者直接修改 ObservationTable：跨线程共享领域状态与授权前沿。
- gap 只放普通 FIFO：队列满时失去失效信息。
- FIFO 自动淘汰：与本阶段计划的满拒绝语义不同，且会引入隐式失效顺序。
- Core 持有媒体指针：生命周期/重启后可访问性无法由引用元数据证明。

## 验证

正常公开 ABI、Mock 字节、现有完整 Native 单元套件、公共 C99/C++ 头/layout 编译及本地静态检查通过，独立审查发现已修正并复查；内存增量已量测。实际命令、结果及 CI 集中在 [#83](https://github.com/RobotSe7en/ayni/pull/83)。未执行新增故障/恢复专项、真实媒体、硬件时限或完整 E1–E4；订阅控制尚待下一 PR。
