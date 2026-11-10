# ADR-0098：共用模型协议、续传与请求工具预置

- **状态**：已接受用户范围（2026-11-10～11）；实现与软件验证见工作分支，PR由用户处理。
- **日期**：2026-11-11。
- **关联**：ADR-0055、0085、0086、0095、0096、0097；[实施计划](../architecture/shared-model-protocol-plan.md)。本决定补充0085的可选工具呈现与协议方式，不改变Core授权、ABI0.54或schema9。

## 背景

ESP实验与Native各自编码Chat请求、解析工具参数，无法共享流式和Responses行为。用户要求复用Ayni接入、遵循OpenAI协议，并把完整工具定义通过每次请求的tools参数预置，继续使用同一个ToolLoop、历史和输入入口。本轮只做软件、验证、提交推送，不烧录、运行硬件、调用付费模型或合并PR。

## 决策

1. **协议归属。** 可选源码SDK组件`Ayni::ModelProtocol`拥有两种wire投影、回答/参数/usage解析、SSE累计和显式工具定义校验；不拥有HTTP、线程、凭据、Core状态或业务执行。Native保留curl与异步发布，ESP保留esp_http_client和任务/队列。更换HTTP实现只改传输；增加普通设备能力只改注册契约/Provider和装配，不改协议业务分支。
2. **显式选择。** 默认仍为Chat Completions、非流式、旧TEXT回复及发现模式。Responses使用input Items、平铺function、call_id/function_call_output、max_output_tokens、store:false及reasoning.encrypted_content；不依赖previous_response_id。Chat输出预算由配置选择max_tokens或max_completion_tokens，不猜模型名；服务扩展不能覆盖协议字段。Responses必须选FRAMED回复；同一Provider和ToolLoop须选择相同回复编码。
3. **正文版本。** 请求封套`AYNI-MODEL-REQUEST/1`只含当前请求工具和消息，不进入历史；回复`AYNI-MODEL-REPLY/1`放在TEXT正文或旧公共工具列表的assistant_text中。普通TEXT不会根据帧标记自行切换解释。历史仍为`AYNI-MESSAGES/1`，有续传的assistant/calls用v2帧把正文与续传绑定。Responses有序output Items与reasoning/encrypted_content保留；从同一响应生成的规范文本、调用ID/函数/原参数与续传在出站时核对，不把任意用户正文提升成续传状态。Chat的FRAMED回复也记录模型/目的地选择。API、模型或目的地不匹配返回UNSUPPORTED或REVISION_CONFLICT，须由调用方显式选择新会话，不自动清空或丢弃续传。
4. **兼容与回退。** 新reader继续读取旧assistant/calls帧和turn格式5，ConversationRecord仍为5，Core/SQLite/文件卷版本不变。turn格式6仅把历史摘要游标改为规范摘要数据的偏移；格式5已在途摘要保留其旧游标和writer直到完成，并拒绝夹带新续传。旧二进制遇格式6须拒绝而非重新执行；回退使用原二进制和原目录/备份，不能让旧writer接管新的FRAMED结果。默认旧消费者无需选择新协议；选择FRAMED/预置的消费者须同时接入编码和投影端口。History/UI只展示规范回答，摘要只收到规范对话/工具事实；删除、撤权失效、清空及压缩按整个历史组移除对应续传。Provider无跨请求续传缓存，终态借用移交后擦除工作副本。
5. **正式预置。** `ToolExposure::preloaded`在decide有效期内经公共snapshot list/inspect读取当前Task可见定义并复制；说明、输入/结果Schema及版本来自实际注册证明。一个通用ayni_call_capability的完整分支Schema覆盖invoke/start和read/wait/cancel/close；UNARY仅invoke、SUBSCRIPTION显式start，保留原操作引用语义。模型不需list/inspect往返，实际调用仍逐次规范化、授权、Scene/PermitGate检查。默认发现模式保留；预置必须注入实际协议Projection，防止工具封装计量与出站分叉。
6. **预算。** 主请求、部分工具组、结果余量、tool回复和两类摘要均用同一协议writer计量，包含完整工具、续传及JSON封装。仍采用ceil(Unicode字符×0.75)，保留用户token设置；实际usage独立报告。注入协议Projection的会话不再把服务token统计与旧Chat字符锚点拼接，使用完整当前投影估算。旧默认Chat消费者保留已有锚点规则。字节容量仍是独立内存限制，不是模型窗口。
7. **流式生命周期。** 共享累计器处理任意字节切分、UTF-8跨块、CRLF、多data行、Chat工具参数片段与末尾usage/DONE，以及Responses原生item/参数事件和completed/failed/incomplete。arguments.done不等于整次完成；成功还须HTTP完整结束。HTTP错误先分类，错误正文不进入预览；无效可选usage不抹掉完整回答。未完成/未知返回ABANDONED/RESULT_UNKNOWN，无自动重试。预览只在I/O worker回调，借用视图活到返回，不得阻塞、重入Core或执行工具；ESP用单独的最新快照通道显示“生成中（预览）”，最终落盘与完成仍一次发生。
8. **ESP设置。** Web和触屏增加API、生成时预览及Chat输出预算字段选择，保存后正常关闭/重启，Key仍遮罩。NVS快照版本2保留版本1reader，旧配置默认Chat/非流式；原Chat目的地身份保持。更换API进入明确的新目的地；没有自动擦卷。DeepSeek thinking仍是显式服务扩展。流式/非流式均将完整选定回复存为BodyRef后才post，不扩大Core内联结果容量。

## 后果

可共享协议和正常矩阵，同时保留设备能力、执行权和传输的各自属主。代价是有状态的模型回复增加正文存储与请求体积；完整预置随目录增长占用预算，容量不足明确拒绝，不摘要工具Schema。最大上下文、PSRAM峰值、线上厂商差异及硬件仍须各自取得证据。Responses首版支持message/output_text/refusal、reasoning和function_call；其他输出类型明确拒绝，不假装支持图像或服务端工具。

## 备选方案

不采用复制第二套ESP工具循环、把Responses仅换URL、丢弃reasoning或依赖Provider私有续传缓存。不给每项设备手抄工具参数，不让模型预览成为可执行提议。旧reader保留以接续现有正常状态，但不承诺旧二进制能够消费新回复格式。

## 验证

两种API的文本/工具、usage、流式分块及多轮配对通过本地合成协议与HTTP夹具检查；真实ToolLoop预置、完整组和不含opaque内容的摘要通过正常软件验证。ESP设置旧reader和正文结果接线纳入同一正常检查；固件编译不代表硬件运行。独立只读审查及修复复查、最终命令/结果以本分支提交说明为准。未进行线上模型、烧录、串口、电机、128K峰值、故障/恢复、变异或耐久活动。

官方协议依据：[function calling](https://developers.openai.com/api/docs/guides/function-calling)、[Responses迁移](https://developers.openai.com/api/docs/guides/migrate-to-responses)、[reasoning](https://developers.openai.com/api/docs/guides/reasoning)、[streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)。
