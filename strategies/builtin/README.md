# 内建策略与对话值

`ayni_strategies` 是 Core 外的 C++17 有界组件，仅用公开接口；消息按需分配，固定暂存仍由调用方workspace提供（ADR-0095）。configure 后借用的配置和策略对象保持至 Kernel 销毁；单 owner 线程调用，不可并发回调或移动。规则策略沿用固定步骤/版本与未知结果暂停语义。

ToolLoop 通过 `Conversation::begin` 生成 Task 参数，默认使用固定四个 `ayni_` function、标准 assistant/tool 消息以及独立协议 ID。source Model Call 和提议序号决定步骤键；目录、完整说明、业务规范化、授权及普通 Call 分别检查，发现不扩权。主模型和摘要均计入既有模型调用上限；默认八次，所有者可以明确提高。

窗口、输出预留、压缩目标与工具结果上限来自运行设置（ADR-0095）：估算输入超过“窗口 − 输出预留 − 工具结果上限”时压缩，压缩后不超过压缩目标；最新完整工具往返受保护。优先服务 usage；预检或缺失统计按消息、固定工具及 JSON 结构的 Unicode 字符数×0.75 向上取整。未改写前缀采用最近实测加新增估值；压缩、删除、来源失效清除锚点。它是明确估值，不保证与厂商 tokenizer 一致。原始结果由共同结果视图保留，超限后分块摘要，保留真实 outcome/effect 和原规范值/证明引用；失败保留原文并暂停。完整 Schema 不允许有损摘要。

上下文帧、历史和摘要按实际长度分配，受token设置、注册契约和正文预算限制，模型请求与回答经正文存储以引用往返（ADR-0096），Task 参数与检查点只含引用和控制字段。单条输入放不进窗口时明确拒绝，无静默截断。公开历史按引用轮次查询，不由SQLite另存逐消息副本。每回调扫描有界模型轮次与提议。

Native/ESP/portable使用共享Session/FileBodies；BodyProgress.ready接入FileBodies::progress，只推进异步正文，队列空闲返回值不作为Task准入条件；当前读写依赖单独DEFER。SQLite只实现通用值端口，不保留旧history/archive回调。DEFER 不提交候选，普通服务 NOT_READY 仍为失败。可选 LocalClock 只向历史追加时间数据，最前部固定指令不随时间改写。

外层存储流程：begin 保留原记录；成功 Task 经公开 checkpoint 持久屏障导出；finish 核对 owner/id/generation/revision；候选存储成功才替换 current。删除提升代际并拒绝迟到结果，原业务执行结论不可被摘要或正文擦除改写。

内部会话记录格式6保存顺序/共享正文引用，turn格式8使用同一具名字段、小端整数、长度与无尾字节规则。Core 存储 schema、会话 codec、SQLite schema 和消息 frame 版本分开，旧格式写前拒绝，无迁移。参考 [ADR-0085](../../docs/adr/0085-stable-capability-tools-and-shell.md)、[Native 历史契约](../../contracts/host/native_conversation.md)。

ADR-0098增加可选`ToolExposure::preloaded`，通过当前Task的公共list/inspect捕获完整注册说明及Schema，生成一次请求的通用调用分支；不把定义写进历史，不另写工具循环。该模式必须提供真实协议Projection；主请求、部分工具组、工具回复和两类摘要计量复用出站writer。注入投影时完整估算与服务usage分开，避免混用不同协议字符锚点。

Provider/ToolLoop显式匹配主/摘要ReplyEncoding；FRAMED在工具列表assistant_text或TEXT正文承载回复封套。continuation绑定对应assistant/calls帧，普通文字不会按标记自动解释。历史摘要只接收规范对话/调用事实，不含opaque reasoning；旧组替换时连同续传一起删除，最新工具组保持完整。当前ConversationRecord6/turn8拒绝旧reader；摘要游标仍指向规范数据，旧二进制与旧目录配套回退。

ADR-0104引入显式Persistence：默认durable，ESP选择background。stage只接受有界RAM正文；保存历史时异步confirm共享payload/index，模型请求完整上下文作为临时输入在原Call终态持久后回收。工具结果只保存规范原值一次，消息索引保存原Schema/domain和小JSON投影视图，冷读复用同一writer；用户原始文本不按magic解释，完整历史由Conversation::history显式读取。回合缓存有界并检查当前来源；工具JSON缓存仅复用不可变表示，每个主请求重新列出可见工具，实际权限仍由Core复核。新格式异常恢复和板端交接目标未验证。

启用ToolLoop的组合根须在每次Core step后、任何历史/隐私ACK前调用`maintain(kernel, privacy)`（owner线程、Core回调外）。它只用公开Task元数据和privacy request_seq清理策略自有表示，取消/失败和隐私失效的WAITING回合不会等到下一次decide才释放。活跃清理保守全清私有回合；同一COMPLETE代际不影响随后有效回合的复用。`cached_turns()`仅报告私有回合数量，不暴露正文。运行时负责取得经管理会话查询的公共privacy进度，Schema渲染缓存仍独立于授权结论。

共享对话主路径沿用输入/模型/顺序工具/最终保存：冷读合并独立依赖后才投影历史，活动回合及下一Task的完全相同来源复用既有有界工作集；新Task仍验证正文所有权/摘要，当前权限和隐私继续逐次检查。每会话最多一份最近完成图与活动图共享cached_turns额度，取消/失败清理工作副本，隐私进度清空全部。Session一次请求完整候选图的confirm并保留全部持久屏障；展示新答案使用latest_answer，完整rounds用于历史查询。见[ADR-0105](../../docs/adr/0105-shared-conversation-flow-and-local-cost.md)。

ADR-0108统一普通/受管结果的小视图：原值、域、证明与可选展示文本；一个活长结果的完整投影供所有摘要块复用，每次决定仍查询当前Core来源，结束/失效立即释放。旧HistoryPort及无限正文缓存已删除，所有部署显式选择正数缓存预算。

ADR-0109将私有摘要文字与引用交给同一属主，替换时明确失效，未变检查点不再散列。历史索引复用新输入/最终答案的原正文，单Writer生成完整请求封套，压缩完成仅在下一请求投影。Session输入stage一次，durable仍先confirm和保存根记录；发布按collected(scope)等待真实清理，关闭仍排空整个存储队列。C ABI与持久格式保持。
