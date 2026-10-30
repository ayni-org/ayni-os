# Native 会话历史与原始结果

- **关联**：ADR-0085；C-POLICY-012、C-MEM-009/010；实现 `runtime/native_node/conversation_*`。
- **属主**：Runtime/Host 拥有 SQLite、worker、缓存与 namespace；Core 只持有小引用、调用事实、原日志与快照。

启动持有原 Native 目录锁，只接受私有普通非链接数据库文件。存在旧 conversation.bin 时在新写入前拒绝并保留目录；SQLite application_id=0x41594E49、user_version=1，不认识版本拒绝。会话 codec3 与消息 frame1 独立于 Core schema6。旧二进制/原目录用于回退，无自动迁移或清空。

启动后一个 worker 独占 SQL，owner callbacks 仅在 mutex 下复制有界作业。state save、history load/save、raw archive 各一 lane，queued/writing 时明确 NOT_READY；全局 ready 覆盖全部共享 lane。SQLite FULL 同步、DELETE journal、foreign_keys、secure_delete 与 trusted_schema=OFF；事务提交后才报告完成并允许 Core 保存引用。ready 与 decide 之间完成用显式 DEFER 处理，不允许静默吞掉服务错误。关闭先停止 Kernel 消费，再排空所有 lane、join worker、关数据库；Core step 不等 SQL。

messages 按不可变消息追加，views 通过有序 message_id 引用重用不变前后缀。加载按 keyset 每页64条；逐行检查 ordinal 及 owner/conversation/generation，工作历史仍受49152 B缓冲限制。长期 SQL 历史总量不受该工作缓冲限制；本轮无总配额或历史浏览 UI。Task 参数/checkpoint 仅存 view 引用和控制状态。

results 以 namespace/task/call/sequence 唯一保存原文、规范值、原授权域/业务契约证明、真实 outcome/status/reason/effect、模型 usage 和 JSON 视图。回调返回前复制借用，提交后才发 result_ref。已有行与当前 Core 原始事实/正文及重新生成 JSON 比较；持久 JSON 不拥有执行事实。原始拷贝约200 KiB、JSON作业1 MiB，分块至多32768 B并保持 UTF-8 边界；容量不足拒绝，不截断。结果摘要失败不删除原行、不重新执行业务动作。

所有读取/写入都检查 owner、conversation、generation；worker 写前再次检查当前 state。删除事务清除旧视图/原始结果和派生消息；单轮删除保留其他独立 user 输入，全删除不保留旧正文。提交后同 mutex 更新 namespace、清除旧缓存及排队作业，再允许 ACK；迟到旧代际读写返回 EXPIRED。这里的 secure_delete 不宣称清除了文件系统快照、备份或设备磨损副本。

正常验证：`ConversationDatabase.appends_messages_once_and_reads_views_across_sql_pages`、`ConversationDatabase.generation_erasure_rejects_old_cache_reads_and_late_writes`、`ConversationDatabase.deleting_one_turn_preserves_other_independent_user_messages`、`NativeAssistant.short_history_and_conversation_survive_clean_close_and_reopen`。新 SQL 路径未执行故障切断/恢复专项，不以现有 Core 历史证据代替。
