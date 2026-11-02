# ADR-0095：七项运行设置与按需分配

- **状态**：已接受。部分取代 ADR-0085 决策 5（75% 触发、50% 目标）与 ADR-0090 中"策略不在 decide 分配"的规定；其余条款不变。
- **日期**：2026-11-02。
- **关联**：ADR-0085、ADR-0090、ADR-0096；`runtime/common/include/ayni/runtime/common/harness_settings.h`。

## 背景

消息链路上原有十余个互相推导的字节上限：384 B 用户输入、512 B 摘要、49152 B 历史缓冲、8192 B 提示词、64 KiB 调用文本、128 KiB HTTP 响应、Provider 的 `text_max_bytes`，以及各组合根手写的信封余量。它们与模型的 token 窗口无关，改一个常常要连带改几处，也让 Ayni 作为通用底座时只能按某个平台的内存量预留。用户确认：应用只配 token、次数、时间与并发，消息链路不再有字节限制，也不再按字节预留内存。

## 决策

1. **七项设置。** `runtime::HarnessSettings` 是唯一来源：`context_window_tokens`（W）、`max_output_tokens`（O）、`compact_target_tokens`、`tool_result_max_tokens`（R）、`max_turns`、`request_timeout_ms`、`model_concurrency`。组合根把它传给 ToolLoop、Provider 与 Core，其他地方不再手写这些数。配置必须满足 W > O > 0、O 不超过 32 位、R < W − O、压缩目标 < W − O − R、`max_turns` 不超过 Core 上限，`request_timeout_ms` 为 1..300000、`model_concurrency` 为 1..8（每个内置模型 Provider 都接受的范围）。Native CLI 用同名配置键；旧键 `output_reserve_tokens` 报明确错误。
2. **行为规则。**
   - 单条用户输入的估算 token 放不进 W − O 时拒绝，返回容量超限，不调用模型。
   - 模型回答与摘要只受 O 约束，没有字节上限。
   - 估算输入超过 W − O − R 时压缩，为下一次工具结果留出空间；压缩后的上下文不超过压缩目标。
   - 单个工具结果超过 R 时走既有归档与分块摘要；没有归档端口时返回容量超限。
   - 上下文因权限变化重置时，从最新一轮起保留用户输入，总估算不超过压缩目标。
3. **按需分配。** 策略层、Provider 与正文存储在运行中按实际长度分配：ToolLoop 的上下文帧经 `model::Writer` 的增长回调写入 `std::string`；OpenAI 兼容 Provider 先测量再一次分配请求，响应按倍增增长并擦除旧缓冲，回答缓冲按响应大小一次分配。Provider 删除 `text_max_bytes` 与 `response_max_bytes`，只保留一个由 O 推出的防御上限（64 KiB 元数据 + 每 token 96 B），超过时按模型输出容量超限失败，不截断。分配器就是平台的全局 `operator new`；需要指定内存区的平台通过它选择。
4. **不变的部分。** Core 仍不分配内存（ADR-0014）。策略声明的控制状态（任务参数、检查点）只含正文引用和控制字段，大小固定，不随消息长度变化。纯函数的工作区查询（ADR-0090）保留，只覆盖与消息长度无关的部分。

## 后果

应用只需理解七个数，消息长度由模型窗口决定。内存紧张的平台可能在运行中途分配失败，此时返回容量超限，用户已确认接受这一风险。压缩触发点从固定比例改为公式，窗口较大时会更晚触发；原 75%/50% 的配置不能原样保留，需要按新含义设置 R 与压缩目标。

## 备选方案

不采用"保留字节上限但改为可配置"：那仍要求应用理解十余个互相推导的数。不采用"启动时按设置预留最大缓冲"：最大消息由窗口决定，按窗口预留会让大窗口配置在小设备上无法启动，而大多数消息远小于窗口。

## 验证

`StrategiesFixture.long_answer_completes_and_is_retained_intact`（超过 384 B 的回答）、`StrategiesFixture.summary_replaces_history_only_after_all_budget_chunks_succeed`（1500 B 摘要、保留输入按 token 截断）、`CompactionTrigger.compaction_starts_past_window_minus_output_minus_tool_result`、`StrategiesFixture.single_input_beyond_the_window_is_refused_without_a_model_call`、`ContextHarness.oversized_executed_result_uses_chunk_summaries_and_keeps_known_facts` 与 `OpenaiProvider.oversized_http_body_is_bounded_model_output_failure`。
