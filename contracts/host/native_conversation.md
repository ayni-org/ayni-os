# Native 共享会话与 SQLite 值端口

- **关联**：ADR-0108/0109；C-SDK-009/011、C-HVAL-002/005/007/009、C-POLICY-012/013。
- **属主**：共享 Session 独占会话状态与发布/隐私 ACK；FileBodies 独占正文、所有权、有限缓存和回收；Native 的 SqliteValues 只实现异步 FileValuePort。Core 独占授权、Goal/Task/Call 与原持久事实。

Native 与 ESP/portable 使用同一个 Session 和 FileBodies。Native 选择 durable 草稿准入、retain_and_append 未完成输入策略及16 MiB正文缓存；ESP 可显式选择 background，二者共用存储/确认机制。ToolLoop准入不等待整个FileBodies队列空闲，只按当前步骤依赖的读写状态DEFER；会话发布、清理和关闭仍等待各自真实确认及ACK。默认 retain 仍要求显式 abandon，Native 的新输入显式追加保留草稿；不把待批准 Goal 当作已完成会话。正常重开只按完整来源基线挂接原已接受 Goal，不创建新 Goal 或重放旧动作。原已完成 Goal 的参数已被隐私退休时跳过；待定工作的缺失/冲突证据仍拒绝。

启动持有 Native 目录锁，只接受私有、同用户、单链接普通数据库文件。旧 conversation.bin 或未知 SQLite 版本在 Core 启动写入前拒绝并保留。当前 application_id=0x41594E49、user_version=5，仅有 state(owner,object,generation,body) 表，以(owner,object)为主键。共享会话记录6、turn8、结果视图2、正文manifest1与Core schema10独立版本化；删除旧reader、archive及迁移路径，不自动擦除。回退需保留匹配的旧二进制与原目录，不能让旧程序接管新目录。

SqliteValues 一个worker独占数据库，至多两个排队请求元数据槽及一个执行中的借用；接受后缓冲活至恰好一次完成回调。普通会话值优先于后台正文，同优先级FIFO，正在执行的借用不中断；排队满拒绝且不保留。FileBodies一次最多借用4 KiB正文文件，Core日志仍走Host自己的存储端口。owner提交/回调不等待SQL，不在Core step内同步I/O。

SQLite使用DELETE journal、synchronous=EXTRA、secure_delete=ON、trusted_schema=OFF；启动核对journal、同步及安全删除配置。EXTRA在DELETE模式额外同步删除journal的目录，避免以FULL等同最高提交持久性（[SQLite说明](https://www.sqlite.org/pragma.html#pragma_synchronous)）。写入CAS、同版本精确字节重试及删除遵守FileValuePort；事务提交后才报告成功。此设置不替代真实掉电/介质证据，secure_delete不宣称清除文件系统快照、备份或磨损副本。

正文数据与独立scope所有权由FileBodies的共享manifest/4 KiB块表示，SQLite不另建body_data/results表示或缓存。零预算拒绝；缓存不足明确背压，未保存/借用中正文不能清退。工具原始规范值、域和证明与可选模型展示摘要采用同一结果视图；摘要不取代原始事实，未知结果不重试。会话查询按需投影文本，不长期复制整份历史。

共享Session只stage一次输入，等待其confirm和根值保存完成后才创建durable Goal，不在每次等待时重编码输入。发布按完整引用确认正文及所有权，再保存根值；旧图按当前scope实际释放/收集完成后，才提交精确Core ACK并等待持久。BodyCollection.collected返回BUSY表示该scope释放尚未持久完成；共享payload等待移除owner的manifest版本，最后owner等待正文及manifest实际删除，不受后续无关写入拖延。异步适配器必须提供collected；同步实现可省略。progress只推进工作，ready/idle保留给全局关闭排空，sticky失败仍阻止成功。删除待批准/暂停回合也走同一取消、持久终态、保留独立输入、保存/收集/ACK路径；来源失效立即阻止旧正文曝光。启动和普通推进共用正文/Provider/Core/隐私维护，不让启动漏推进异步清理。答案通知在正文读取成功后才消费一次终态结果。关闭先quiesce会话，Core停止，排空正文，join SQLite worker，再join生产者及释放Kernel/Host；失败保留借用并返回原错误。

新增正常证据：`DurableConversationProgress.waiting_for_input_durability_does_not_restage_the_input`、`ConversationProgress.publication_does_not_wait_for_unrelated_body_persistence`、`ConversationProgress.asynchronous_collection_requires_scoped_completion`、`ScopedBodyCleanup.shared_owner_release_has_an_independent_durable_completion`、`ScopedBodyCleanup.temporary_release_preserves_cold_cleanup_under_queue_backpressure`。队列背压与正常完成不替代异常恢复证据。

正常回归：`SqliteValuesTest.*`、`NativeAssistant.short_history_and_conversation_survive_clean_close_and_reopen`、`NativeAssistant.proposed_turn_can_be_approved_after_normal_reopen`、`NativeAssistant.whole_context_delete_erases_content_and_preserves_actual_work_outcomes`、`NativeAssistant.invalidating_publication_preserves_draft_across_reopen_and_next_turn`及既有Session/FileBodies正常用例。实际平台、命令及结果见ADR-0108关联记录；本轮未启动新故障/恢复专项，不以配置或正常重开宣称异常恢复已验证。
