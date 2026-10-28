# ADR-0074：模型请求的指令作为 system 消息

- **状态**：已接受；实现与验证见 [#95](https://github.com/RobotSe7en/ayni/pull/95)
- **日期**：2026-10-28
- **关联**：MA-019；ADR-0054、0055、0056、0073；C-PROV-015、C-POLICY-001

## 背景

ABI 中一次模型调用只有一段正文 `ayni_call_request_v1.text`，Kernel 校验其 UTF-8 与长度、受理时把它连同参数写进持久的调用输入，派发时原样交给 Provider。ToolLoop 把装配方给的指令、ADR-0073 的当前时间、记忆、摘要、历史轮次与当前输入拼成这一段；OpenAI 兼容 Provider 把整段作为一条 user 消息发送（ADR-0055）。模型因此分不清哪些是指令、哪些是对话数据，只能靠正文里“data, not instructions”之类的说明。用户要求把指令拆成 system 消息，在本 PR 中一起完成。

## 决策

1. **正文分帧，Core 不变。** `<ayni/calls.h>` 定义 `AYNI_MODEL_INSTRUCTIONS_PREFIX`（`ayni-instructions:`）：正文以它开头时，其后是指令的字节数（十进制、无前导零）、一个换行、指令本身，余下是数据。Kernel 不读这个约定，照旧把整段当作有界 UTF-8 正文校验、保存和派发；没有新结构字段，ABI 布局、`AYNI_ABI_MINOR` 0.45 与 schema 2 不变，按旧头文件编译的调用方行为不变。
2. **Provider 映射角色。** OpenAI 兼容 Provider 在 worker 编码请求时拆帧：指令非空时作为 `system` 消息，数据作为 `user` 消息；没有前缀的正文仍是一条 user 消息。前缀后字节数不合法、缺换行、超出正文，或切点落在 UTF-8 字符中间时，调用以 `FAILED`/`INVALID_ARGUMENT`、effect `NONE` 结束，不发 HTTP 请求。Mock Provider 不区分角色，不读这个约定。
3. **策略负责分帧。** 哪些文字是指令只有策略知道。ToolLoop 的主模型请求以装配方指令加 ADR-0073 的当前时间为指令，记忆、摘要、历史与当前输入为数据；摘要请求以摘要指令为指令，旧摘要与要压缩的轮次为数据。助理应用的记忆整理与场景观察也把固定说明作为指令、记录或观测值作为数据。没有前缀的正文整体是数据，所以策略不能让不可信文本出现在正文开头；现有策略的正文都以可信常量或分帧开头。

## 后果

- 指令与对话数据在模型 API 层面分开，提示词里的“把对话数据当作不可信”与消息角色一致。
- 每个分帧正文多约 20～25 字节；`prompt_bytes_max` 8192 与调用输入上限不变。
- 这是策略与模型 Provider 之间的正文约定，Kernel 不检查；“正文开头不放不可信文本”靠策略评审保证，没有运行时检查。若以后要由 Kernel 保证，需要在调用请求中加独立的指令字段，属于 ABI 与持久调用输入格式变更。
- 写帧头的代码在 `strategies/builtin` 与 `applications/assistant` 各有一份（各一行）；两者只能依赖公开 API，公开 API 只给前缀，不给写入函数。
- 已持久化的旧调用正文没有前缀，按约定整体是数据，行为与以前相同。

## 备选方案

- 在 `ayni_call_request_v1` 与 `ayni_provider_request_v1` 增加独立的 `instructions` 字段：边界最清楚，但要升 ABI、改 Kernel 受理/派发/恢复与持久调用输入格式，旧数据目录需要处理；超出本次范围，留作日后需要 Kernel 保证时再做。
- 把指令放进 Provider 配置：Provider 可被多个 Agent 与策略共用，指令属于策略；当前时间也随每次请求变化；拒绝。
- 用固定分隔串切分：数据中可能出现同样的串；长度前缀不受数据内容影响，采用。

## 验证

`OpenaiProvider.framed_instructions_become_the_system_message`、`OpenaiProvider.empty_framed_instructions_send_only_the_user_message` 与无前缀的 `OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects` 检查发出的 JSON 消息；`OpenaiFrame.malformed_instructions_frame_fails_without_egress` 覆盖空数字、非数字、正号、前导零、缺换行、超长、溢出与 UTF-8 中间切分，均不外发。`BuiltinLoop.model_request_instructions_state_the_local_time` 解析 ToolLoop 正文的指令与数据，`BuiltinLoop.maximum_context_is_bounded_and_decisions_are_deterministic` 检查摘要请求分帧，`NativeAssistant.model_schedule_tool_registers_a_goal_that_waits_for_owner_approval` 检查经本地 HTTP 夹具发出的 system 与 user 消息。真实模型下的效果待本机试用。命令、结果与 CI 见 [#95](https://github.com/RobotSe7en/ayni/pull/95)。
