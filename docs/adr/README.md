# 架构决策记录（ADR）

本目录记录 Ayni 的重大架构决策。规范性规则以实施计划 [`docs/ayni_v1.0.md`](../ayni_v1.0.md) 的对应章节为准；ADR 记录背景、决策、取舍、备选方案与验证方式，不重复全部规则，避免多处维护同一条款。

## 状态

- **提议**：候选方案，须在记录注明的时点前接受、修订或否决；未决定前不得写成既定设计。
- **已接受**：作为设计基线写入实施计划；不代表已实现或已验证，实现与验证证据另见 `reports/`。
- **已取代**：由后续 ADR 取代；修改已接受决策的语义时新增 ADR，不改写历史记录。

## 索引

| 编号 | 标题 | 状态 | 计划章节 |
|---|---|---|---|
| [0089](0089-independent-source-sdk.md) | 独立源码 SDK 与跨平台消费边界 | 已接受，实施中 | §1.1、§8 |
| [0001](0001-device-governance-and-execution-permits.md) | 设备治理模型与执行许可 | 已接受 | §2.2、§2.6、§2.10、§7.3 |
| [0002](0002-commit-and-output-gating.md) | 提交与输出门控协议 | 已接受 | §2.4、§2.8、§4.3 |
| [0003](0003-trust-model-and-adapter-confinement.md) | 信任模型与适配器约束 | 已接受 | §2.5、§3.7 |
| [0004](0004-fabric-identity-freshness-idempotency.md) | 跨设备身份、新鲜度与幂等 | 已接受 | §3.1、§3.4 |
| [0005](0005-esp32-layered-stop-chain.md) | ESP32 分层停止链 | 已接受 | §4.1、§7.1、§7.3 |
| [0006](0006-kernel-lifecycle-ports-and-policy-interface.md) | Kernel 生命周期、Host/Provider 边界与策略接口 | 已接受 | §2.0、§2.5、§3.2、§4.4 |
| [0007](0007-notification-presentation-and-task-state.md) | 通知呈现复用能力调用与 Task 状态维度 | 已接受 | §2.4、§5.4 |
| [0008](0008-core-modules-by-invariant-owner.md) | Core 模块按不变量属主划分（两级目录，2026-10-19 修订） | 已接受 | §2.0、§2.3、§8 |
| [0009](0009-idl-and-local-api-encoding.md) | IDL 与本地 API 编码 | 提议（本地 API 形态已于 2026-10-19 接受） | §3.1、§3.2 |
| [0010](0010-composition-root-and-targets.md) | 组合根 `runtime/` 与可执行目标 `targets/` | 已接受 | §2.6、§8、§11 |
| [0011](0011-build-layout-and-dependency-checks.md) | 构建布局与依赖检查 | 已接受 | §2.0、§2.3、§8、§11 |
| [0012](0012-test-framework.md) | 测试框架 | 已接受 | §10.1、§10.6 |
| [0013](0013-contract-format-before-idl.md) | IDL 选定之前的契约表达方式 | 已接受 | §3.1、§3.2 |
| [0014](0014-core-does-not-allocate.md) | Core 不自行分配内存 | 已接受 | §3.2、§4.2、§8 |
| [0015](0015-ingress-lock-and-completion-slots.md) | 入口临界区、有界事件队列与完成槽 | 已接受 | §3.2、§4.2 |
| [0016](0016-boot-recovery-and-bounded-step.md) | 启动恢复、持久 boot_epoch 与有界 step | 已接受 | §3.2、§4.2、§4.3 |
| [0017](0017-work-model-principals-agents-goals.md) | 工作模型第一部分：主体与会话、Agent 实例、GoalType 与 Goal 意图 | 已接受 | §2.2、§2.4、§3.2、§3.6、§5.3 |
| [0018](0018-tasks-and-propagation.md) | 工作模型第二部分（一）：Task、派生与传播 | 已接受 | §2.2、§2.4、§3.2、§4.2、§5.3、§9.1 |
| [0019](0019-time-triggers.md) | 工作模型第二部分（二）：延时与周期触发 | 已接受 | §2.1、§2.2、§4.4、§5.3、§14.1 |
| [0020](0020-goal-expiry-and-bounded-waits.md) | 工作模型第二部分（三）：Goal 失效与有界等待 | 已接受 | §2.2、§2.4、§4.4、§5.3 |
| [0021](0021-local-input-events.md) | 工作模型第二部分（四）：事件投递与本地输入触发 | 已接受 | §2.1、§2.2、§3.2、§4.2、§5.3 |
| [0022](0022-strategies-and-decisions.md) | 工作模型第三部分（一）：策略与决策 | 已接受 | §2.4、§2.5、§2.7、§3.2、§4.2、§5.3 |
| [0023](0023-goals-end-with-their-last-task-or-occurrence.md) | Goal 随最后一个 Task 的取消或最后一个 occurrence 的跳过结束 | 已接受 | §2.2、§2.4、§5.3 |
| [0024](0024-steps-and-private-checkpoints.md) | 工作模型第三部分（二）：步骤与私有检查点 | 已接受 | §2.4、§2.5、§3.2、§4.2、§5.3 |
| [0025](0025-persistence-split-and-terminal-goals-give-way.md) | 持久化与恢复（一）：拆分与终态 Goal 让位 | 已接受 | §2.8、§4.2、§9.1 |
| [0026](0026-work-journal-replay-native-file-log-and-commands.md) | 持久化与恢复（二）：工作日志、重放、Native 文件存储与命令幂等 | 已接受 | §2.8、§4.2、§4.3、§9.1 |
| [0027](0027-retries-writer-fences-snapshots-and-migration.md) | 持久化与恢复（三）：重试、写入栅栏、快照与迁移 | 已接受 | §2.8、§4.2、§4.3、§4.4、§9.1 |
| [0028](0028-trusted-owner-binding-and-sessions.md) | 权限与审批（一）：可信所有者绑定与会话入口 | 已接受（A 已合并 #25，本地验证与 CI 通过） | §2.2、§2.4、§2.5、§3.2、§4.3、§5.3、§9.1、§14.1 |
| [0029](0029-capability-domains-and-scope-algebra.md) | 权限与审批（二）：Capability 目录与有限范围代数 | 已接受（B 已合并 #26，Native 验证、独立审查与最终八项 CI 通过） | §2.2、§2.3、§3.2、§4.2、§9.1、§14.1 |
| [0030](0030-grants-delegation-and-exact-command-identity.md) | 权限与审批（三）：Grant、受限委派与确切命令身份 | 已实现并完成 Native 验证（C 五配置784/784、49检出+1等价；#27 已合并（`b133032`），代码与最终文档 CI 各八项全过） | §2.2、§2.3、§2.5、§2.8、§3.2、§4.2～4.4、§9.1、§14.1 |
| [0031](0031-revocation-gates-and-intent-receipts.md) | 权限与审批（四）：即时撤销、拒绝栅栏与阶段回执 | 已接受（#28 已合并） | §2.2、§2.4、§2.8、§3.2、§4.2～4.4、§9.1、§14.1 |
| [0032](0032-readability-checks-and-review-evidence.md) | 可读性机械检查与复查交付证据 | 已接受（R1 本地验证与独立审查通过） | §9.1、§14.1 |
| [0033](0033-kernel-id-and-grant-record-ownership.md) | Kernel ID 与 Grant 记录基础规则归属 | 已接受（R2a1；验证见正文） | §9.1、§14.1 |
| [0034](0034-field-values-and-scope-admission.md) | 字段值编解码与 Scope 准入结果复用 | 已接受（R2a2；验证见正文） | §9.1、§14.1 |
| [0035](0035-grant-rules-and-bounded-chain-admission.md) | Grant、命令与 Kernel 编排的状态归属 | 已接受（R2b；验证见正文） | §9.1、§14.1 |
| [0036](0036-unconfirmed-goal-control-guard.md) | 未确认授权的 Goal 普通控制守卫 | 已接受（R3；验证见正文） | §9.1、§14.1 |
| [0037](0037-goal-requirements-and-atomic-approval.md) | Goal 需求、待批与原子批准 | 已接受（E；覆盖率偏差见正文） | §9.1、§14.1 |
| [0038](0038-work-authority-and-explicit-reauthorization.md) | 工作授权、显式重新授权与正常重启 | 已接受 | §9.1、§14.1 |
| [0039](0039-native-idle-space-refresh.md) | Native 空闲空间刷新 | 已接受 | §9.1 |
| [0040](0040-shared-storage-batch-identity.md) | 批次身份解析归属 | 实施中 | §9.1 |
| [0041](0041-task-record-encoding-owner.md) | Task 记录编码归属 | 实施中 | §9.1 |
| [0042](0042-runtime-limits-and-direct-model-access.md) | 运行限制、直连模型及 M1 剩余交付边界 | 已接受；实施中 | §2.3、§5.1、§9.1、§14.1 |
| [0043](0043-task-runtime-limit-state.md) | Task 运行限制状态与所有者提高上限 | 已接受 | §9.1、§14.1 |
| [0044](0044-provider-result-mailbox.md) | Provider 有界结果交接与可靠消费确认 | 已接受 | §2.7、§9.1、§14.1 |
| [0045](0045-unified-call-state.md) | 统一调用身份、动作事实与派发消费 | 已接受 | §2.7、§2.8、§4.3、§9.1 |

| [0046](0046-resource-topology-and-conflict-domains.md) | 可信资源拓扑与冲突域 | 已接受 | §2.2、§9.1 |

| [0047](0047-trusted-work-scheduling.md) | 可信工作分类与资源等待策略 | 已接受 | §9.1 |
| [0048](0048-reusable-mock-providers.md) | 可复用确定性 Mock Provider | 已接受 | §9.1 |

| [0049](0049-local-executor-permit-gate.md) | 独立执行端许可门 | 已接受 | §2.10、§9.1 |

| [0050](0050-managed-execution-and-scenes.md) | 受管理执行、Scene 与停止闭环 | 已接受 | §2.10、§9.1 |

| [0051](0051-kernel-structure-refactor.md) | Kernel 结构整改 | 已接受 | §2.0、§8 |

| [0052](0052-remote-executor-and-observation-input.md) | 远程执行端与观测输入的线程模型 | 已接受 | §2.10、§9.1 |

| [0053](0053-m1-structure-wrap-up.md) | M1 结构收尾：主版本 0 只收当前 ABI 布局、精确命令查找、调用传播与共享默认配置 | 已接受 | §3.2、§8、§14.1 |

| [0054](0054-typed-tool-calls.md) | 类型化工具调用：允许列表 ∩ 授权、Provider 侧解码与 v4 调用行 | 已接受；`call_tool` 读取由 0096 取代 | §2.7、§3.3、§9.1 |

## 模板

新 ADR 使用下一个编号，并包含以下小节：状态、日期、关联（计划章节、工作包、实验）、背景、决策、后果、备选方案、验证。提议状态的 ADR 另写明决定时点与决定依据。
| [0055](0055-openai-compatible-provider.md) | Native OpenAI-compatible Provider、有界 JSON 与取消/排空 | 已接受 | §2.7、§3.3、§9.1 |

| [0056](0056-builtin-strategies-and-conversations.md) | 规则策略、ToolLoop、有界对话与检查点 reader 声明 | 已接受 | §2.7、§3.3、§9.1 |

| [0057](0057-native-dual-agent-cli.md) | 双 Agent Native/Sim 装配、对话文件与单进程 CLI | 已接受，P2 已合并；双 Agent 与点名由 0072 取代 | §3.2、§9.1、§14.1 |

| [0058](0058-kernel-service-runtime-boundaries.md) | Kernel 私有状态、service/runtime 分区、StepContext 与 CallbackGate | 已接受 | §2.3、§3.2、§9.1 |

| [0059](0059-grant-restore-and-persistent-stores.md) | Grant 事实/撤销闸门/恢复职责与 Grant、命令持久适配 | 已接受 | §2.3、§2.8、§9.1 |

| [0060](0060-private-memory-and-lineage.md) | 有界私有记忆、来源权限、Context 文件屏障与逻辑隐私清理 | 已接受 | §2、§9.1、§14.1 |

| [0061](0061-trusted-utc-and-calendar.md) | 可信 UTC、POSIX 日历规则与稳定发生时刻 | 已接受 | §2.2、§9.1、§14.1 |

| [0062](0062-observation-ingress-and-mock-media.md) | 有界观测入口、可靠失效前沿与 Mock 媒体 | 已接受 | §2.2、§3.6、§9.1 |

| [0063](0063-task-subscription-leases.md) | Task 所有订阅、有限租期、可靠控制与新样本等待 | 已接受 | §2.2、§3.6、§9.1 |

| [0064](0064-work-persistent-stores.md) | Agent、Goal 与 Task 的持久存储属主 | 已接受 | P5、F04 |

| [0065](0065-persistent-pause-groups.md) | 通用持久暂停组 | 已接受 | P5、MA-019 |

| [0066](0066-assistant-application-and-presentation.md) | 统一助理应用、四类目标与异步通知呈现 | 已接受；路由/合并由 0072 取代 | §5.4、§9.1、§14.1 |

| [0067](0067-cpp-sdk-and-public-examples.md) | 无异常 C++ 生命周期封装与四个公开接入示例 | 已接受 | §9.1、P5 |

| [0068](0068-normal-integration-fixtures.md) | 正常集成夹具、编译数据与脱敏轨迹 | 已接受 | §10.1～10.5、P6 |

| [0069](0069-native-capacity-measurements.md) | Native 容量测量与编码开销口径 | 已接受 | §4.2、P6、F18 |

| [0070](0070-typed-multinode-normal-profile.md) | typed 多节点正常子集与固定服务 Task | 已接受（服务 Task 暂按推荐项） | §3.4、P6、MA-038 正常子集 |

| [0071](0071-normal-integration-closeout-and-coverage-gap.md) | 正常集成收尾与覆盖率差距 | 已接受 | P6、§4.2、§9.1 |

| [0072](0072-single-assistant-schedule-tool-and-stop.md) | 单一助理、模型计划工具与确定性停止 | 已接受；“问用户日期”由 0073 取代 | §5.4、§9.1、§14.1 |

| [0073](0073-model-request-local-time.md) | 模型请求写明当前本地时间 | 已接受 | §2.5、§5.4、§14.1 |

| [0074](0074-model-instructions-as-system-message.md) | 模型请求的指令作为 system 消息 | 已接受 | §2.5、§5.4、§14.1 |

| [0075](0075-full-experiment-runner-and-fault-controls.md) | 完整实验运行器与受控故障生命周期 | 已接受 | §10.1、MA-021/022/023/033 |

| [0076](0076-e2-recovery-and-executor-evidence.md) | E2 精确提交切断与独立执行端证据 | 已接受 | §10.3、MA-022 |

| [0077](0077-e1-shared-service-acceptance.md) | E1 共享服务完整验收与来源失效恢复 | 已接受 | §10.2、MA-021 |

| [0078](0078-e3-provider-and-policy-replacement.md) | E3 重启后替换模型与兼容策略 | 已接受 | §10.4、MA-023 |

| [0079](0079-e4-composite-and-version-acceptance.md) | E4 组合、订阅依赖与版本拒绝 | 已接受 | §10.5、MA-033 |

| [0080](0080-safety-branch-coverage-closeout.md) | 安全范围分支覆盖收尾 | 已接受 | §9 M1、§10.6 |

| [0081](0081-capability-centered-agentos-evolution.md) | 通用能力接入与 AgentOS 架构演进 | 提议 | §2、§3、§6、§8、§9 |

| [0082](0082-prepublication-cleanup-and-capability-delivery.md) | 预发布清理、存储统一与能力自描述交付 | 已接受 | §2、§4、§9、AP-00/AP-01 |

| [0083](0083-general-foundation-before-platform-experiments.md) | 通用 AgentOS 基础先于平台实验 | 已接受 | §0、§1、§6、§9～§15、AP-02～AP-11 |

| [0084](0084-typed-capability-values-and-authority-projection.md) | 通用业务值、完整授权投影与开发期升级 | 已接受范围；实现未交付 | §3、§9、§14、AP-02 |

| [0085](0085-stable-capability-tools-and-shell.md) | 固定能力工具、按需说明、连续上下文与显式 Shell | 已接受范围；实施中；决策 5 由 0095 取代 | AP-02/AP-03 |

| [0086](0086-task-owned-managed-operation-controls.md) | Task 所有长操作与订阅的模型控制 | 已接受产品选择；实施设计 | §2、§3、§9、§14、AP-05 |
| [0087](0087-binding-resource-mapping-and-foundation-acceptance.md) | Binding 资源映射与基础验收边界 | 已接受；验证与交付见 #117 | AP-04/AP-06、基础验收 |
| [0088](0088-safety-branch-coverage-target.md) | 安全关键分支覆盖目标统一为 80% | 已接受 | 仓库规则、工作流、基础及发布验收 |
| [0090](0090-bounded-portable-workspaces.md) | 通用有界工作区与低栈接口 | 已接受；实现与验证见 #119；策略分配规定由 0095 取代 | ABI0.53、独立接入 |
| [0091](0091-shared-host-file-storage.md) | 共用Host文件存储与平台文件端口 | 已接受；实现与验证见 #120 | 独立接入、C-HOST-009～019 |
| [0092](0092-esp-idf-littlefs-host.md) | FreeRTOS异步Host与专用LittleFS卷 | 已接受；实现与验证见 #121 | 独立接入、C-HOST |
| [0093](0093-host-outer-file-values.md) | 外层文件值与共同Host队列 | 已接受；实现与验证见 #122 | C-HVAL、独立接入 |
| [0094](0094-portable-runtime-and-independent-normal-program.md) | 共同会话运行时与独立正常程序 | 已接受范围；验证见 #123 | 独立SDK/平台接入 |
| [0095](0095-token-settings-and-on-demand-buffers.md) | 七项运行设置与按需分配 | 已接受；部分取代 0085 决策 5 与 0090 | ToolLoop、Provider、组合根 |
| [0096](0096-core-stores-body-references.md) | 正文存储与 Core 只存引用 | 已接受；部分取代 0054、0085、0090 的正文规定 | ABI0.54、schema9、Provider、组合根 |
| [0097](0097-esp32-touch-wifi-voice-experiment.md) | 独立 ESP32 触屏 Wi-Fi 语音配置 | 已接受实验范围；完整实机验收未完成 | 独立实验、API、RC003-MS、Web 设置 |

| [0098](0098-shared-model-protocol-and-preloaded-tools.md) | 共用模型协议、续传与请求工具预置 | 已接受；软件验证见工作分支，PR由用户处理 | ModelProtocol、ToolLoop、Native/ESP、正文帧 |
| [0099](0099-esp32-web-ota.md) | 独立ESP32双槽与Web OTA | 已接受用户选择；正常更新与未验证边界见实验记录 | 独立实验、分区兼容、Web管理 |
| [0100](0100-shared-interactive-runtime.md) | 共享交互会话、正文缓存与提交辅助 | 已接受实施范围；用户后续安排独立审查 | runtime/conversation、HostStorage、C++ SDK、ESP实验 |
| [0101](0101-touch-apps-and-automatic-reply-voice.md) | 触屏应用入口、自动语音与电机启用 | 已接受用户选择；正常证据与限制见实验记录 | 独立ESP实验、UI偏好、受管语音、局部启停 |
| [0102](0102-reuse-call-proofs-and-storage-accounting.md) | 复用调用契约与简化正常存储路径 | 已接受用户选择；软件验证与实机边界见实验记录 | Core调用链、共享FileLog、ESP空间账目 |
| [0103](0103-reuse-immutable-admission-in-live-calls.md) | 运行期复用不可变准入并接收多工具批次 | 已接受用户选择；软件验证与实机边界见实验记录 | Core契约读取、执行门、ESP多工具串行 |

| [0104](0104-shared-conversation-bodies-and-dispatch-prefix.md) | 共享对话正文、异步保存与连续派发前缀 | 已接受用户选择；本轮仅软件实现与编译，板端暂缓 | ABI0.55、schema10、ToolLoop、正文与Host队列 |

| [0105](0105-shared-conversation-flow-and-local-cost.md) | 共享对话主路径与本地重复工作 | 已实施并正常OTA；局部验证与未完成边界见实验记录 | 共享历史、Session、Native/ESP接入；ABI/schema不变 |
| [0106](0106-bounded-body-reads-and-publication.md) | 操作内正文复用与会话发布确认 | 已实施，本地正常验证及独立复查通过；不OTA | 共享策略/Session；C ABI及持久格式不变 |
