# 共用模型协议实施计划

- **范围**：用户2026-11-10～11要求ESP32复用Ayni模型接入，支持OpenAI兼容Chat Completions、Responses及流式，并完成工具预置；沿用真正内置ToolLoop，不另写实验循环。
- **状态**：完整软件实现、相关正常验证与独立复查已完成。历史检查点为`cc51c64`，本次续作继续同一分支；验证细节见提交说明。PR由用户处理。
- **分支**：`feat/esp32-preloaded-tools`，基于用户合并#130/#131后的main。不要恢复旧WIP方案或整体cherry-pick可选容量分支。
- **边界**：本轮不烧录、不连接或复位串口、不运行电机、不擦卷、不调用付费线上模型、不合并PR。编译和本地夹具不代表线上厂商、128K峰值或硬件验收。

## 已保留的用户决定

- 一套内置ToolLoop、历史和输入入口。模型决定直接回答或调用工具；最终文字显示聊天记录，语音由模型调用扬声器。
- 完整定义通过标准tools参数随当前请求传递，每请求一份，不逐轮追加历史；不再让模型list/inspect往返。
- 上下文128×1024估算token、输出预留1800、摘要目标4096、单工具结果预算4096；满时允许额外摘要。字节容量独立约束内存。
- 原生OpenAI协议，不把Responses当Chat换URL，不猜测或修补工具参数；提议仍通过Core授权、Scene和PermitGate。

## 实施结果与属主

1. **共用协议**：可选`Ayni::ModelProtocol`拥有两种API投影、回答/工具参数/usage解析、有界JSON和SSE。Native保留curl、取消及发布，ESP保留esp_http_client和本机任务；不依赖Core私有头、Key或线程后端。固定yyjson源码导入仍共用。
2. **原生API**：Chat使用messages、嵌套function和tool_call_id；Responses使用input Items、平铺function、call_id/function_call_output、max_output_tokens、store:false与reasoning.encrypted_content。Chat输出字段显式选择max_tokens/max_completion_tokens，不按模型名猜测；DeepSeek thinking保持显式扩展。
3. **续传正文**：请求工具用AYNI-MODEL-REQUEST/1，仅属于请求BodyRef；回复用显式ReplyEncoding和AYNI-MODEL-REPLY/1，工具调用继续用公共工具列表的assistant_text。历史assistant/calls v2将续传绑定到对应组。Responses完整有序Items及encrypted reasoning与规范文本/调用从同一响应产生并核对。FRAMED Chat也绑定model/scope；跨API/模型/目的地明确拒绝，不自动清空或丢弃状态。
4. **正式预置**：ToolExposure::preloaded在decide借用期内通过公共snapshot list/inspect复制当前Task获准的真实注册说明及完整输入/结果Schema。一个ayni_call_capability分支Schema保留invoke/start/read/wait/cancel/close，原Core逐调用检查保持。默认旧消费者仍为发现模式。
5. **真实计量**：预置必须注入实际Protocol::projection；主请求、部分工具组、结果余量、工具回复及两类摘要共用出站writer，包含定义和续传。结果摘要追加也检查相同预算。注入投影后完整估算与reported usage分开，不沿用旧Chat字符锚点；默认旧Chat保留原规则。
6. **流式与预览**：共享累计器接受任意字节切分、CRLF、多data行和UTF-8跨块，处理Chat工具参数/finish/末尾usage/DONE与Responses原生item/参数/终态事件。arguments.done不能触发执行，成功须完整协议终态及HTTP完成。预览在worker执行，只是暂定文本，不能重入Core或执行工具。HTTP及明确输出拒绝保留分类，中断/未知为ABANDONED/RESULT_UNKNOWN，不自动重试。
7. **ESP接线**：Web和触屏“设置→模型选项”提供API、生成时预览和Chat预算字段选择。配置冻结后正常保存/关闭/重启，Key仍遮罩；NVS2读取NVS1默认Chat/非流式，旧Chat目的地身份保持。完整选定回复经BodyRef交接，不提高Core内联结果容量。预览在取消、换轮、清空和终态时清理。

## 兼容、隐私与回退

架构决定、消费者及回退分析见[ADR-0098](../adr/0098-shared-model-protocol-and-preloaded-tools.md)。Core ABI0.54/schema9、ConversationRecord5及SQLite/文件卷不变；turn6只将摘要游标解释为规范摘要数据偏移，继续读取turn5。已在途旧摘要保留旧writer直至完成；旧二进制须拒绝turn6，不能接管新的FRAMED结果。默认旧TEXT消费者不按文字中的标记选择解码。

History/UI只显示规范回答。压缩源是规范对话/调用事实，不向摘要模型发送opaque或encrypted Items；最新工具往返保留，旧组完成替换时一并移除其续传。撤权失效、清空和重建复用原披露栅栏与整个历史组生命周期。Provider没有跨请求续传缓存，终态移交后擦除并释放工作副本。回退保留原二进制和原目录/备份，不自动擦除或迁移用户卷。

## 软件验证与证据边界

相关正常验证包含两种API×流式/非流式的文本/工具/usage矩阵，本地curl HTTP/SSE、逐字节分块、参数拼接、有序reasoning多轮、完整工具预置、不累积定义、规范摘要、实际投影预算、旧设置读取和错误分类。保留既有策略、操作、对话及Wi-Fi回归；独立协议SDK工程和ESP-IDF6.0.3编译均通过。Web截图来自本地合成配置，见[设置预览](../../experiments/esp32_wifi_chat/screenshots/model-settings-preview.png)。

具体命令、结果、工具链和独立复查分项写入本次提交说明，PR由用户创建/处理；本轮没有创建PR或触发线上服务。未运行新故障、恢复、断电、耐久或变异活动，未执行任何硬件操作；其他Native配置和完整CI未据本地检查推定为通过。后续线上、完整128K峰值、60秒动作和独立停止链仍分别等待用户安排。

## 后续复验入口

先检查主机内存、swap、CPU和其他作业，设置正整数CMAKE_BUILD_PARALLEL_LEVEL与CTEST_PARALLEL_LEVEL，再选择相关工程：

- `tests/independent/model_protocol`：公开SDK协议工程，不选择Linux-only Host。
- 根工程`ayni_model_protocol_tests`、`ayni_openai_provider_tests`、`ayni_strategies_tests`：协议、Native传输和真实ToolLoop。
- `experiments/esp32_wifi_chat/tests/wifi`：便携适配器、设置及既有Wi-Fi正常检查。
- `tests/portability/sdk_import_test.py`：只读隔离SDK消费者。
- 独立实验`firmware`：准备固定字体后用ESP-IDF6.0.3仅编译，不运行deploy。

缓存、build日志、ESP-IDF环境、私密会话、仓库外ASR/TTS服务不随Git推送。不要把已撤回实验专用预置方案的历史42项检查当作本实现证据。

官方依据：[function calling](https://developers.openai.com/api/docs/guides/function-calling)、[Responses迁移](https://developers.openai.com/api/docs/guides/migrate-to-responses)、[reasoning](https://developers.openai.com/api/docs/guides/reasoning)、[streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)。
