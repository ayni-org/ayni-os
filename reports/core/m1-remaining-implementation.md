# M1 剩余能力实施顺序

依据用户 2026-10-25 批准的计划与 [ADR-0042](../../docs/adr/0042-runtime-limits-and-direct-model-access.md)；2026-10-26 用户在 M1 详细实施计划中选定先做文本纵切，顺序按 ADR-0042 决定 11 分为 P0～P6，原第 4～12 行重排如下。当前交接入口仍为 [m1-progress.md](m1-progress.md)。每行是可审查的完整能力，前置清理/编码/API/测试用独立提交组织；不因正常测试通过宣称专项验收完成。

| 顺序 | 阶段 | 能力 | 实施及验收边界 | 状态 |
|---|---|---|---|---|
| 0 | — | 产品约束 | 废止费用额度，记录直连 API、公平性、持久暂停、满表拒绝及测试边界 | 已随 #60 合并 |
| 1 | — | MA-018A | 可信静态资源目录；运行健康/占用/隔离；全有或全无；Agent 轮转、实例 FIFO；等待原因/期限；Task 持久调用次数与所有者提高上限 | 资源机制见 [#60](https://github.com/RobotSe7en/ayni/pull/60)；运行限制/所有者提高上限见 [#61](https://github.com/RobotSe7en/ayni/pull/61)（ADR-0043）；调用消费已由 #64 接通；Device/Component/冲突域见 [#65](https://github.com/RobotSe7en/ayni/pull/65)；工作分类/等待策略/诊断见 [#66](https://github.com/RobotSe7en/ayni/pull/66) |
| 2 | — | MA-008A | 统一调用与稳定步骤身份，Binding 校验，ActionRecord、PREPARED/DISPATCHING 持久屏障，最终复核；进度/取消/终态可靠完成槽；B4 原子应用；Mock A/B；排空关闭 | 内部结果交接已合并 #62（ADR-0044）；内部调用契约已合并 #63，公开调用链/完成/对账/等待恢复已合并 #64（ADR-0045）；可复用 Mock A/B 已由 #67 交付 |
| 3 | — | MA-018B | 过程与控制会话、nonce 锚定续租、到期不可恢复、即时授权门控、最小 Scene；停止确认前隔离；独立模拟执行端/停止链 | 独立门 #68（ADR-0049）；Core、Scene、最小观察和 Mock 停止闭环见 [#69](https://github.com/RobotSe7en/ayni/pull/69)（ADR-0050）；执行端端口异步化见 [#71](https://github.com/RobotSe7en/ayni/pull/71)（ADR-0052）；正常功能通过，故障/恢复与硬件专项待验收 |
| 4 | P0 | 结构收尾 | 审查项 F05 泛型精确命令查找、F06 调用对 Task 的传播移到 work、F11 主版本 0 只收当前 ABI 布局、F16 前半共享默认配置、F19 文档/草案对齐；两个 runner 的并行 CI；行为、ABI 0.36 布局与 schema 2 不变 | [#72](https://github.com/RobotSe7en/ayni/pull/72)（ADR-0053） |
| 5 | P1 | MA-008C | typed 工具调用结果（至多 8 项）与工具视图，GoalType 允许列表内已授权的部分，ABI 0.37；OpenAI-compatible Provider：libcurl multi 工作线程、yyjson 有界解析、运行时 endpoint/model/密钥环境变量名，非流式文本与工具调用，无自动重试；本地协议夹具，实 API 单独验证 | Core 侧 typed 工具调用、工具视图与允许列表见 [#73](https://github.com/RobotSe7en/ayni/pull/73)（ADR-0054，ABI 0.37）；OpenAI-compatible Provider 未开始 |
| 6 | P1 | MA-009 | 规则和 ToolLoop 两策略；单轮最多 8 调用、按序；命名版本步骤；未知阻止后续；检查点明确兼容；补偿新调用新授权；多轮对话保存最近轮次与模型摘要，P1 只删整个对话 | 未开始 |
| 7 | P2 | MA-020 前半 | runtime 演示装配（F16 后半）与 SimNode；单进程 Native CLI 内嵌 Kernel，命令映射已有意图；显式点名对 A/B 说话；脚本化 Mock CLI 会话含重启继续 | 未开始 |
| 8 | P3 | 结构整改 | F01 `ayni_kernel` 成员私有化与 service/runtime 分区，F14 StepContext，F08 作用域重入守卫；行为、ABI 与 schema 不变 | 未开始 |
| 9 | P3 | MA-011A | 私有命名空间与所有者公共档案；来源/版本/确认状态；有限检索与上下文 lineage；tombstone 立即拒绝传递派生依赖、有界擦除；单轮对话删除使摘要失效；满表拒绝；随同 F04（Grant/命令）、F12、F10 | 未开始 |
| 10 | P4 | MA-011B | 可信 UTC 独立于 monotonic；单次/每日/每周与有界时区规则；不可信待校时；稳定 occurrence；DST/跳时补发至多一次 | 未开始 |
| 11 | P4 | MA-008B | 订阅 open/renew/close、代际/序号/gap、TTL/质量、媒体引用；任意线程观测队列（ADR-0052 决策 6，F15）；新代际与依赖复核；可靠控制回执 | 未开始 |
| 12 | P5 | MA-019 | 文本回合与提醒/设备检查/记忆整理/场景观察；通用持久暂停组、可信应用归类、新工作同样受限；通知成功呈现才完成、安静时段；随同 F04 完成 | 未开始 |
| 13 | P5 | MA-020 后半 | C++17 包装 `ayni.hpp`、显式排空/关闭；四个接入/组合示例只用公开 API，CI 编译运行；双 Agent 演示 | 未开始 |
| 14 | P6 | E1–E4 正常子场景 | 可复现 JSON 夹具/轨迹；公平/私有/限制、正常屏障、策略/Provider 切换与组合；独立子用例 ID，不冒充原完整实验 | 未开始 |
| 15 | P6 | MA-038 正常子集 | 多独立 Kernel/时钟/存储、typed FakeTransport、固定 owner 的正常远程调用及查询；IDL/解码器和故障矩阵另候条件 | 未开始 |
| 16 | P6 | 收尾 | F18 最大容量实测（超预算才改）；覆盖率、ci:full、默认/最小/最大内存；M1 正常集成报告（不是 Core Alpha） | 未开始 |

对话数据决定（用户 2026-10-25）：持久保存当前 Task 所需的有界输入/回答正文以便继续；删除接入记忆与隐私机制，不保存密钥或原始媒体，不建立无限历史归档（ADR-0042 补充决定）。

其余用户决定（2026-10-26，ADR-0042 决定 12～16）：ABI 主版本 0 只收当前布局；Native CLI 单进程内嵌 Kernel；Provider 把工具调用转成 typed 结构，Core 不见 JSON；模型可见工具为 GoalType 允许列表内已授权的部分；旧对话轮次由一次计入 Task 模型上限的模型摘要替代，写入成功后才丢弃。

实现通则：复用现有授权目录、命令幂等、提交协调和记录 codec；不从模型接受驱动/资源锁或任意端点。Provider 线程只向预留槽投递，不能直接改 Core。可靠完成记录遇存储背压保留，不被 progress 覆盖。Task 普通恢复不重置调用计数；提高软限额不超过启动硬容量。默认模型槽 1、Task 模型调用 8、请求超时 30 秒；前台等待超限暂停、提醒可明确无界等待、后台跳过有计数；低容量配置可禁用尚不使用的表。

完成定义：每项有契约、实现、正常测试、作者设计自查和适用独立审查；本地按相关行为选择检查，CI 运行既有 ASan/UBSan 全套。ABI/容量变化量 arena，硬件数字另测。完整 E1–E4、Core Alpha、故障恢复、ESP32 烧录、Web/语音/屏幕均不能由本表的普通集成替代。
