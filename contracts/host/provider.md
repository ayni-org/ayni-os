# Provider 接口

- **关联**：计划 §2.7、§3.2、§3.3「新模型」、§3.6、§5.1；ADR-0006；`MA-008`、`MA-027`；E1 步骤 2、4，E2 F4–F7，E3 步骤 2、6，E4 步骤 1、3
- **属主**：`catalog/binding`（声明与匹配）、`work/call`（调用实例与尝试）；各 Provider 实现（`providers/`）
- **头文件**：`include/ayni/draft/provider_v1.h`

Provider 实现 Capability：外部模型 API、感知、媒体、检索、动作执行等。Provider 是可信内置代码（C-TRUST 表），但其**输出**是不可信数据，由 Kernel 校验后才影响状态（C-TRUST-001）。Provider 不持有领域对象的写权限，不调度任务。

当前实施边界（ADR-0044）：内部 `ProviderMailbox` 已实现有界复制、进度合并与取消/终态保留，尚未装入 Kernel；本页公开 Provider 接口仍是草案。终态发表封闭该 attempt 的投递，先前已接受的取消控制先消费；读取不释放，业务提交准入后确认才释放。普通对话所需有界正文将作为受权限/删除约束的业务数据持久保存（ADR-0042 补充决定）；C-PROV-004 禁止的是错误与诊断日志泄露。

## 函数表

| 成员 | 含义 | 调用线程 |
|---|---|---|
| `struct_size`、`provider_id`、`version` | 身份与版本 | — |
| `ctx` | 调用方所有，保持有效至 `ayni_destroy` 返回 `AYNI_OK` | — |
| 能力声明 | 每个实现的能力：调用形态、输入/结果 Schema 引用、模态、副作用属性（`calls_and_bindings.md`）、费用类别、外发目的地、取消与查询属性、执行截止 | 注册时复制 |
| `submit(ctx, request)` | 非阻塞提交一次尝试 | Kernel 所有者线程 |
| `cancel(ctx, request_ref)` | 请求取消 | Kernel 所有者线程 |
| `query(ctx, operation_ref)` | 按 `operation_id` 查询结果（只在声明了查询属性时提供） | Kernel 所有者线程 |
| 结果事件 | 进度、终态、取消结果、查询结果、可选 usage 诊断事实 | Provider 线程经 `ayni_provider_post` 写入该尝试的完成槽 |

## 结果的形式

| 事件 | 字段 | 说明 |
|---|---|---|
| 进度 | `(boot_epoch, request_id)`、`sequence`、进度载荷 | `action` 调用可选；按序号去重；尚未应用的进度只保留最新一条 |
| 终态 | `(boot_epoch, request_id)`、`COMPLETED` / `FAILED` / `ABANDONED`、结构化结果或原因码、可选 usage 诊断事实、证据引用 | 每个尝试恰好一个终态被应用；`ABANDONED` 表示 Provider 不再跟踪且不知道结果，Kernel 记为 `RESULT_UNKNOWN` |
| 取消结果 | `CANCELLED`（终态）/ `CANNOT_CANCEL` / `UNKNOWN` | 不代表物理停止（C-STOP-006）；`CANNOT_CANCEL` 与 `UNKNOWN` 保留到被应用，后到的进度不覆盖它 |
| 查询结果 | `FOUND`（附状态）/ `NOT_ACCEPTED` / `UNKNOWN` | `NOT_ACCEPTED` 只有声明 `AUTHORITATIVE` 时才有效 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-PROV-001 | Provider 只在 `CONFIGURING` 注册；函数表与能力声明在注册时复制并校验限额；`ctx` 由调用方保持有效至 `ayni_destroy` 返回 `AYNI_OK` | 运行中注册新 Provider，或注册后修改能力声明使其与 Binding 不一致 | `MA-005`；C-LIFE-003 |
| C-PROV-002 | `submit` 非阻塞，同步返回遵守 C-HOST-004 的三种结果；业务结果只经事件回送 | 模型 Provider 在 `submit` 内同步等待 HTTP 响应 | `MA-008`；C-CALL-003 |
| C-PROV-003 | 终态携带与能力结果 Schema 一致的结构化结果；Kernel 先做 Schema 校验再做语义校验，不合格的结果按失败处理并记录原因 | 模型返回自由文本“已完成”，Kernel 直接把 Task 标为完成 | `MA-008`；E1 步骤 9；C-TRUST-001 |
| C-PROV-004 | 厂商错误映射为状态码与原因码；厂商原始错误文本、凭据与请求正文不进入 `ayni_error_v1`、日志或用户可见文案 | 错误详情中包含带 API Key 的请求 URL | 评审；C-STATUS-006 |
| C-PROV-005 | 只有声明 `AUTHORITATIVE` 查询且保证记录完整的 Provider 才能返回 `NOT_ACCEPTED`；其余 Provider 查不到记录时返回 `UNKNOWN` | 不保存历史的 Provider 对未知请求返回 `NOT_ACCEPTED`，Kernel 据此重新派发 | E2 查询夹具；C-CALL-010 |
| C-PROV-006 | 取消以事件报告 `CANCELLED`、`CANNOT_CANCEL` 或 `UNKNOWN`；取消结果在被应用前不被后到的进度覆盖；`CANCELLED` 不表示未计费或未外发 | 本地连接取消后，Provider 把远端已执行的可能性报告为从未接受；或 `CANNOT_CANCEL` 之后紧跟一条进度，Kernel 只看到进度，Task 一直停在 `CANCELLING` | 内部交接部分已实现：`ProviderMailboxTest.retains_cancel_and_terminal_until_the_owner_can_commit_each`、`ProviderMailboxTest.terminal_supersedes_progress_without_overwriting_accepted_cancel_control`、`ProviderMailboxTest.concurrent_progress_and_control_posts_preserve_both_channels`；真实取消/业务应用仍待 MA-008；E1 步骤 4；C-CALL-009 |
| C-PROV-007 | 终态可携带 usage/token/bytes 诊断信息；缺失时明确未知，不形成费用账本；Task 调用计数来自本机接受事实（ADR-0042） | 把缺失 usage 伪造为 0 或据此重置 Task 调用计数 | MA-008；C-RUN-002、C-RUN-006 |
| C-PROV-008 | Provider 不调用 Kernel API（`ayni_post_event` 与 `ayni_provider_post` 除外），不修改领域对象，不持有资源锁，不调度任务 | 感知 Provider 直接创建一个“场景观察” Goal | 评审；`providers/` 建立时在 `tools/architecture.json` 登记为只能包含公开头文件，由 `check_deps` 执行（ADR-0011） |
| C-PROV-009 | 模态按能力逐项声明；不因“兼容聊天 API”而假定支持图像、ASR、TTS 或流式输出 | 文本 Provider被绑定到视觉分析能力，运行时才发现不支持图像 | E4 步骤 1；C-BIND-001 |
| C-PROV-010 | 流式输出是需单独协商的可选特性；未协商时 Provider 只返回完整终态 | v0.1 Provider 推送部分 token，Kernel 把部分结果当终态应用 | `MA-008` |
| C-PROV-011 | 直连外部 API；endpoint/model/密钥引用运行时配置，密钥由 Provider/Host 持有，不进入 Core、日志或提交，不绕过本地权限（ADR-0042） | 模型输出改变 endpoint 或读取密钥 | `MA-027`；E3 步骤 6（M2 真实外部 API）；§5.1 |
| C-PROV-012 | 测试用 Mock A / Mock B 是确定性的：相同输入、夹具种子与 FakeClock 轨迹下产生相同结果与完成时刻 | Mock 使用真实时间与随机数，E3 轨迹无法复现 | E1 步骤 2；E3 步骤 1–5 |
| C-PROV-013 | 执行物理动作的 Provider 把指令交给执行端许可门，不直接写 PWM 或驱动寄存器；许可校验由执行端完成（C-PERMIT-007） | 运动 Provider 收到准入结果后直接设置 PWM 占空比 | `MA-015`；R1 B5 |
| C-PROV-014 | Provider 的输入只含 Kernel 按上下文规则过滤后的数据；媒体以受控引用传入（C-PRIM-005），不传任意 URL 或文件路径 | Provider 收到 `file:///data/photos/` 路径并读取全部照片 | `MA-015`；C-WIRE-011 |
