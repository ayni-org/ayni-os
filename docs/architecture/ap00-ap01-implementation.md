# AP-00/AP-01 实施清单

- **决定**：用户 2026-10-29 批准，[ADR-0082](../adr/0082-prepublication-cleanup-and-capability-delivery.md)。
- **顺序**：清理入口 → 统一 Core 格式 → 能力自描述；每部分独立 PR 和证据。
- **范围**：清理、代码迁移和必要局部重构；不保留旧接口/描述/存储兼容或迁移器。M2、硬件、付费 API 和变异仍暂停。

## 1 入口清理

首包实现、公共头/版本/未知输入验证和独立审查见 [#105](https://github.com/RobotSe7en/ayni/pull/105)。

删除四份已替代草案与 Provider 旧路径，消费者统一正式接口；未知值测试保留覆盖但不依赖草案常量。Host trace 草案有独立规划用途，继续留在 draft。ABI 0.46，正式结构布局和 schema 2 不变。

## 2 Core 存储统一

当前实现及格式/恢复/独立审查证据见 [#106](https://github.com/RobotSe7en/ayni/pull/106)。日志与快照使用 schema 3。每个记录族/上下文只保留一种当前正文；相同正文共用版本，不同正文使用不同版本。将历史版本里的授权/来源/清理/调用形态等有效语义改成显式标记，删除旧 reader/writer、版本选择器及旧包装。

记录族仍由原不变量属主编码/校验；Kernel 保留跨记录原子配对；durability 只管封装、CRC 和持久前缀。保留有界分页读写，未知类型/版本/标志拒绝恢复，失败无新写入或外部输出。旧数据保留，不转换或清空；开发改用新目录。

同步字节布局、预算、内存测量、Host/Sim、记录探针和夹具。旧格式拒绝用冻结合成字节，不保留旧 writer 作测试工厂。复用既有运行器验证本次变化必需的提交/快照切断与原子批次拒绝，其他专项不扩展。

## 3 能力自描述

实现及独立审查/验证见 [#107](https://github.com/RobotSe7en/ayni/pull/107)，公开契约见 [tool_usage.md](../../contracts/host/tool_usage.md)。ABI 0.47，schema 3 不变。公开 C99 模型中立说明类型和只读访问端口；按精确 Binding/Capability 版本和 field_id 配对。扩展拥有用途、参数解释、text 文法、结果和示例；runtime 配对和管理生命周期，Core 不存长说明。

每工具说明总计至多 4 KiB、每字段说明至多 256 B；工具数/字段数沿用现有上限。超限或必需说明失配拒绝外发，无截断、legacy 开关或 binding ID 回退。模型 Provider 在 submit 内读取、合并并复制到请求槽，worker 不回调 Core/说明来源。

迁移 schedule 文法和日期用法，提示词保留产品规则；保留当前 parser 和 tool_N/field_N 精确回译。以 schedule 软件能力及模拟设备经本地 HTTP 模型夹具验证，继续走既有授权/资源/Call。完整 Native 应用编排 AP-06 后续独立实施。

## 4 验证与交付

每包相关本地检查与正常行为测试、独立审查及最终适用 CI。存储包增加已授权的必要故障/恢复验证。构建前按宿主和其他工作预算设置正数并行度/容器限额；容量变化测量，不把历史 Native 数字当 ESP 预算。

已接受的旧 ADR/报告保留历史含义，当前契约、计划、追踪和交接随实际交付更新；新的实现状态和限制在对应 PR 记录。

三部分代码与消费者迁移已交付，后续审查修复见 #108/#109。后续统一按[基础能力实施与实验计划](foundation-and-experiment-plan.md)与 [ADR-0083](../adr/0083-general-foundation-before-platform-experiments.md)：先补 AP-02/AP-03/AP-05/AP-06 等基础通用能力及 AP-04 多实例验证，再开展 ESP32-S3、Linux 系统等独立实验。语音、视觉、运动与协议 backend 按需求选用，不规定宿主设备集合；当前没有启动后续代码或平台实验。
