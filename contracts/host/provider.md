# Provider 接口

- **关联**：计划 §2.7、§3.2、§3.3「新模型」、§3.6、§5.1；ADR-0006；`MA-008`、`MA-027`；E1 步骤 2、4，E2 F4–F7，E3 步骤 2、6，E4 步骤 1、3
- **属主**：`catalog/binding`（声明与匹配）、`work/call`（调用实例与尝试）；各 Provider 实现（`providers/`）
- **头文件**：`include/ayni/calls.h`（草案头仅保留 include 桥接）

Provider 实现 Capability：外部模型 API、感知、媒体、检索、动作执行等。Provider 是可信内置代码（C-TRUST 表），但其**输出**是不可信数据，由 Kernel 校验后才影响状态（C-TRUST-001）。Provider 不持有领域对象的写权限，不调度任务。

当前实施边界（ABI 0.38，ADR-0044/0045/0048/0054/0055）：ProviderMailbox 已装入 Kernel，Provider/Binding 注册及文本 unary/action 通过公开接口消费。Host 完成槽与 Provider 槽分池，Provider 不能耗尽存储控制容量。读取不释放，业务提交准入后确认才释放。完成发表结束该 attempt 的投递，但 runtime 仍须 join 生产者线程后再销毁 Core/ctx；Kernel 不能替未协作线程证明退出。有界 Task 正文进入业务记录，凭据、原始媒体及诊断错误原文不进入。

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
| C-PROV-003 | 终态携带与能力结果 Schema 一致的结构化结果；Kernel 先做 Schema 校验再做语义校验，不合格终态结束本机借用，但保留 RESULT_UNKNOWN 并记录 PROVIDER_RESULT 原因，不伪造未执行 | 模型返回自由文本“已完成”，Kernel 直接把 Task 标为完成 | `MA-008`；E1 步骤 9；C-TRUST-001；工具调用列表的语义复核：`ToolCalls.unoffered_tools_and_values_outside_the_offer_end_unknown`（C-CALL-014） |
| C-PROV-004 | 厂商错误映射为状态码与原因码；厂商原始错误文本、凭据与请求正文不进入 `ayni_error_v1`、日志或用户可见文案 | 错误详情中包含带 API Key 的请求 URL | 评审；C-STATUS-006 |
| C-PROV-005 | 只有声明 `AUTHORITATIVE` 查询且保证记录完整的 Provider 才能返回 `NOT_ACCEPTED`；其余 Provider 查不到记录时返回 `UNKNOWN` | 不保存历史的 Provider 对未知请求返回 `NOT_ACCEPTED`，Kernel 据此重新派发 | E2 查询夹具；C-CALL-010 |
| C-PROV-006 | 取消以事件报告 `CANCELLED`、`CANNOT_CANCEL` 或 `UNKNOWN`；取消结果在被应用前不被后到的进度覆盖；`CANCELLED` 不表示未计费或未外发 | 本地连接取消后，Provider 把远端已执行的可能性报告为从未接受；或 `CANNOT_CANCEL` 之后紧跟一条进度，Kernel 只看到进度，Task 一直停在 `CANCELLING` | 内部交接部分已实现：`ProviderMailboxTest.retains_cancel_and_terminal_until_the_owner_can_commit_each`、`ProviderMailboxTest.terminal_supersedes_progress_without_overwriting_accepted_cancel_control`、`ProviderMailboxTest.concurrent_progress_and_control_posts_preserve_both_channels`；公开取消/关闭：`ManagedCall.cancellation_waits_for_terminal_and_late_success_does_not_reopen_task`、`ManagedCall.normal_shutdown_requests_cancel_and_waits_for_returned_borrow`；E1 步骤 4；C-CALL-009 |
| C-PROV-007 | 终态可携带 usage/token/bytes 诊断信息；缺失时明确未知，不形成费用账本；Task 调用计数来自本机接受事实（ADR-0042） | 把缺失 usage 伪造为 0 或据此重置 Task 调用计数 | MA-008；C-RUN-002、C-RUN-006 |
| C-PROV-008 | Provider 不调用 Kernel API（`ayni_post_event` 与 `ayni_provider_post` 除外），不修改领域对象，不持有资源锁，不调度任务 | 感知 Provider 直接创建一个“场景观察” Goal | 评审；已登记 Provider 实例规则并由 `check_deps` 执行；`tests/tools/test_check_deps.py` 的 Provider 自身头/runtime 装配允许与跨 Provider/Core 私有头拒绝用例（ADR-0011/0048） |
| C-PROV-009 | 模态按能力逐项声明；不因“兼容聊天 API”而假定支持图像、ASR、TTS 或流式输出 | 文本 Provider被绑定到视觉分析能力，运行时才发现不支持图像 | E4 步骤 1；C-BIND-001 |
| C-PROV-010 | 流式输出是需单独协商的可选特性；未协商时 Provider 只返回完整终态 | v0.1 Provider 推送部分 token，Kernel 把部分结果当终态应用 | `MA-008` |
| C-PROV-011 | 直连外部 API；endpoint/model/密钥引用运行时配置，密钥由 Provider/Host 持有，不进入 Core、日志或提交，不绕过本地权限（ADR-0042） | 模型输出改变 endpoint 或读取密钥 | `MA-027`；E3 步骤 6（M2 真实外部 API）；§5.1 |
| C-PROV-012 | 测试用 Mock A / Mock B 是确定性的：相同输入、夹具种子与 FakeClock 轨迹下产生相同结果与完成时刻 | Mock 使用真实时间与随机数，E3 轨迹无法复现 | 正常实现：`MockProvider.deferred_completion_copies_text_and_reports_action_progress`；固定推进顺序、A/B 一/两次推进延迟（ADR-0048）；完整 E1/E3 尚未验收 |
| C-PROV-013 | 执行物理动作的 Provider 把指令交给执行端许可门，不直接写 PWM 或驱动寄存器；许可校验由执行端完成（C-PERMIT-007） | 运动 Provider 收到准入结果后直接设置 PWM 占空比 | `MA-015`；R1 B5 |
| C-PROV-014 | Provider 的输入只含 Kernel 按上下文规则过滤后的数据；媒体以受控引用传入（C-PRIM-005），不传任意 URL 或文件路径 | Provider 收到 `file:///data/photos/` 路径并读取全部照片 | `MA-015`；C-WIRE-011 |

## 查询与退出边界

`query(ctx, operation_id, attempt, completion)` 仅接收保存的原 operation 与独立查询 attempt；Provider 注册的 ID/version、端点、目的地与原契约必须精确匹配。未知结果不能改换实现。BEST_EFFORT 的查询仅提供诊断，不能解除原执行 UNKNOWN；AUTHORITATIVE 服务还须对 NOT_ACCEPTED 保证旧 operation 已被封闭。查询的进度/取消控制只用于有界 transport 排空，业务事实由查询终态给出。

`ayni_begin_shutdown` 停止新准入，请求在途取消并等待每项借用终态。取消端口返回 REJECTED_NOT_RETAINED 仅表示本次取消未被接受，不能释放原请求。正常可写退出持久保存事实；已受限且存储不可写时仅排空易失交接，不声称结果已提交。自动重载保留未归还的 transport/完成槽；旧 boot 的结果不直接改变新工作事实，后续需新管理查询。故障、重载及传播竞态仍属暂停的专项证据。

## 工具视图与类型化结果（ABI 0.37，ADR-0054）

结果格式为 `TOOL_CALLS` 的模型调用，`ayni_provider_request_v1` 带 `tool_count` 与 `tool(tool_context, index, out)`：Provider 在 `submit` 内按下标读取每个可提议工具的视图（Binding 与 Capability 身份、正文上限、最多 8 个字段的类型、单位与取值），视图写入 Provider 提供的存储。回调只在本次 `submit` 内、在其线程上有效，任何 `submit` 之外返回 `AYNI_INVALID_STATE`；Provider 不保留它，因为在之后另一次 `submit` 期间它描述的是那次调用的工具。哪些工具可提议由 Kernel 决定（C-CALL-014）。

Provider 自己把模型的工具调用映射到这些视图。未知工具、无法解码的参数、视图外的值或超出工具上限的正文，报告 `FAILED` 与 `AYNI_REASON_MODEL_OUTPUT`（状态为 `INVALID_ARGUMENT`，列表超出结果上限为 `CAPACITY_EXCEEDED`），不当作调用列表发表；仍发表的会被 Kernel 当作不合格终态（C-PROV-003）。`COMPLETED` 终态的 `tool_calls` 与 `result` 文本二者择一；Kernel 在 `ayni_provider_post` 内复制并编码列表，post 返回后 Provider 可复用其数组；编码超出该调用的 `result_text_limit` 时 post 返回 `AYNI_CAPACITY_EXCEEDED`，向文本格式调用发表列表返回 `AYNI_INVALID_ARGUMENT`，借用都保持，Provider 应改报 `FAILED`。模型原始 JSON、函数名映射与厂商 API 细节留在 Provider 内，不进入 Core。

## 可复用 Mock 的当前边界

`providers/mock` 提供 A/B 文本回显与模拟呈现，启动分配固定槽，调用线程只复制输入。组合根在 Kernel 步进之间显式推进；终态成功发表后清空局部正文，`close` 对在途借用返回 `BUSY`。上下文在 Kernel 成功销毁后才能释放。取消、容量和正常排空分别由 `MockProvider.cancellation_drains_the_borrow_and_close_refuses_while_pending`、`MockProvider.full_provider_queue_rejects_without_discarding_the_first_request`、`MockProvider.normal_shutdown_cancels_pending_work_and_returns_every_borrow` 验证。`tools` 服务是结果格式为 `TOOL_CALLS` 的确定性模型：输入开头的 `call:<binding_id>` 词各成为一个已提供工具的调用，字段取视图中的第一个值，其余文本为每个调用的正文；没有这类词时回显文本。未提供的工具、正文超出工具上限或列表超出结果上限都报告 `MODEL_OUTPUT`，由 `MockTools.proposes_the_offered_tool_at_its_first_values_and_the_proposal_runs`、`MockTools.input_without_call_words_is_echoed_as_text`、`MockTools.unoffered_tool_fails_as_model_output` 验证。Mock 接受 Core 已检查过的类型化参数但不使用它们。Mock 不执行网络或真实音频/动作，也不宣称物理停止。

## Native Chat Completions（ADR-0055）

`providers/openai_compat` 用一个 libcurl multi worker 提供非流式文本与 function 工具提议。
公开调用链的正常协议验证：`OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects`、
`OpenaiProvider.offered_function_becomes_exact_typed_values`、
`OpenaiProvider.malformed_json_is_model_output_failure`、
`OpenaiProvider.oversized_http_body_is_bounded_model_output_failure`、
`OpenaiProvider.timeout_returns_borrow_without_retrying_unknown_execution`、
`OpenaiProvider.shutdown_cancels_http_and_drains_the_borrow`。
HTTP 分类与非法参数、列表数量/编码上限由 `OpenaiHttpError`、`OpenaiArguments`、`OpenaiListLimit`
参数化夹具验证。日常测试只使用回环 HTTP，不需要密钥或外部模型。

配置在启动时复制，密钥只由 Provider 从指定环境变量读取；HTTPS 校验保持开启，
只有数字回环夹具允许 HTTP。拒绝 URL 用户信息、query、fragment，不跟随重定向，
不用环境代理/netrc，不自动重试或更换参数；对应验证为
`OpenaiConfiguration.rejects_unsafe_urls_and_missing_key_without_starting_io` 与
`OpenaiProvider.redirect_does_not_send_credentials_to_another_server`。
工具声明、解码与错误规则沿用 ABI 0.37 章节；JSON 深度 8、每容器 64 项，固定解析池。
取消先发表 CANCEL_UNKNOWN，再 ABANDONED 归还借用；HTTP/模型错误的 effect 为 UNKNOWN，
不能由错误码推断从未外发或未计费。正常排空后，组合根须 join worker 才能释放上下文。
独立手动 DeepSeek 验证见 [#75](https://github.com/RobotSe7en/ayni/pull/75)；它不代替新的故障/恢复专项或完整 E1–E4。
