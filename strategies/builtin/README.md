# 内建策略与对话值

`ayni_strategies` 是 Core 外的 C++17 有界组件，仅用公开接口；消息按需分配，固定暂存仍由调用方workspace提供（ADR-0095）。configure 后借用的配置和策略对象保持至 Kernel 销毁；单 owner 线程调用，不可并发回调或移动。规则策略沿用固定步骤/版本与未知结果暂停语义。

ToolLoop 通过 `Conversation::begin` 生成 Task 参数，默认使用固定四个 `ayni_` function、标准 assistant/tool 消息以及独立协议 ID。source Model Call 和提议序号决定步骤键；目录、完整说明、业务规范化、授权及普通 Call 分别检查，发现不扩权。主模型和摘要均计入既有模型调用上限；默认八次，所有者可以明确提高。

窗口、输出预留、压缩目标与工具结果上限来自运行设置（ADR-0095）：估算输入超过“窗口 − 输出预留 − 工具结果上限”时压缩，压缩后不超过压缩目标；最新完整工具往返受保护。优先服务 usage；预检或缺失统计按消息、固定工具及 JSON 结构的 Unicode 字符数×0.75 向上取整。未改写前缀采用最近实测加新增估值；压缩、删除、来源失效清除锚点。它是明确估值，不保证与厂商 tokenizer 一致。原始结果先归档，超限后分块摘要，保留真实 outcome/effect/result_ref 与 unconfirmed_summary 标记；失败保留原文并暂停。完整 Schema 不允许有损摘要。

消息链路没有字节上限：上下文帧、历史和摘要按实际长度分配，模型请求与回答经正文存储以引用往返（ADR-0096），Task 参数与检查点只含引用和控制字段。单条输入放不进窗口时明确拒绝，无静默截断。应用预览保留四轮，与 SQLite 长期逐消息历史分开。每回调扫描有界模型轮次与提议。

Native 用 `HistoryPort` 复制有界作业、返回 NOT_READY，并通过 ready/DEFER 非阻塞等待；SQL 在 Host worker 执行。load/save 必须配套，archive/chunk 成对；Sim 可内联历史。DEFER 不提交候选，普通服务 NOT_READY 仍为失败。可选 LocalClock 只向历史追加时间数据，最前部固定指令不随时间改写。

外层存储流程：begin 保留原记录；成功 Task 经公开 checkpoint 持久屏障导出；finish 核对 owner/id/generation/revision；候选存储成功才替换 current。删除提升代际并拒绝迟到结果，原业务执行结论不可被摘要或正文擦除改写。

内部会话记录格式5只存正文引用，turn格式6使用同一具名字段、小端整数、长度与无尾字节规则。Core 存储 schema、会话 codec、SQLite schema 和消息 frame 版本分开，旧格式写前拒绝，无迁移。参考 [ADR-0085](../../docs/adr/0085-stable-capability-tools-and-shell.md)、[Native 历史契约](../../contracts/host/native_conversation.md)。

ADR-0098增加可选`ToolExposure::preloaded`，通过当前Task的公共list/inspect捕获完整注册说明及Schema，生成一次请求的通用调用分支；不把定义写进历史，不另写工具循环。该模式必须提供真实协议Projection；主请求、部分工具组、工具回复和两类摘要计量复用出站writer。注入投影时完整估算与服务usage分开，避免混用不同协议字符锚点。

Provider/ToolLoop显式匹配主/摘要ReplyEncoding；FRAMED在工具列表assistant_text或TEXT正文承载回复封套。continuation绑定对应assistant/calls帧，普通文字不会按标记自动解释。历史摘要只接收规范对话/调用事实，不含opaque reasoning；旧组替换时连同续传一起删除，最新工具组保持完整。ConversationRecord仍为5，turn格式6的摘要游标指向规范数据；reader5保留，在途旧摘要维持旧编码直到结束。旧二进制不能接管新帧，回退保留原目录。
