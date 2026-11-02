# Native 会话历史与原始结果

- **关联**：ADR-0085；C-POLICY-012、C-MEM-009/010；实现 `runtime/native_node/conversation_*`。
- **属主**：Runtime/Host 拥有 SQLite、worker、缓存与 namespace；Core 只持有小引用、调用事实、原日志与快照。

启动持有原 Native 目录锁，只接受私有普通非链接数据库文件。存在旧 conversation.bin 时在新写入前拒绝并保留目录；SQLite application_id=0x41594E49、user_version=3，不认识版本拒绝。会话记录格式 5（只含正文引用）独立于 Core schema。旧二进制/原目录用于回退，无自动迁移或清空。

启动后一个 worker 独占 SQL，owner callbacks 仅在 mutex 下复制有界作业。state save、history load/save、raw archive 各一 lane，queued/writing 时明确 NOT_READY；全局 ready 覆盖全部共享 lane。SQLite FULL 同步、DELETE journal、foreign_keys、secure_delete 与 trusted_schema=OFF；事务提交后才报告完成并允许 Core 保存引用。ready 与 decide 之间完成用显式 DEFER 处理，不允许静默吞掉服务错误。关闭先停止 Kernel 消费，再排空所有 lane、join worker、关数据库；Core step 不等 SQL。

bodies 表以正文引用 id 为主键保存 scope、digest 与原文：对话文本按会话记录 id、模型请求与结果按各自作用域写入（ADR-0096）。写入去重，事务提交前回答 NOT_READY；读取校验 digest。无挂起轮次的保存会回收记录不再引用的本作用域正文。正文长度只受 token 设置限制，不再有工作历史缓冲；Task 参数/checkpoint 只存正文引用和控制状态。

results 以 namespace/task/call/sequence 唯一保存原文、规范值、原授权域/业务契约证明、真实 outcome/status/reason/effect、模型 usage 和 JSON 视图。回调返回前复制借用，提交后才发 result_ref。已有行与当前 Core 原始事实/正文及重新生成 JSON 比较；持久 JSON 不拥有执行事实。原文按需分配、不截断，摘要分块按 tool_result_max_tokens 并保持 UTF-8 边界。结果摘要失败不删除原行、不重新执行业务动作。

所有读取/写入都检查 owner、conversation、generation；worker 写前再次检查当前 state。删除事务清除旧视图/原始结果和派生消息；单轮删除保留其他独立 user 输入，全删除不保留旧正文。提交后同 mutex 更新 namespace、清除旧缓存及排队作业，再允许 ACK；迟到旧代际读写返回 EXPIRED。这里的 secure_delete 不宣称清除了文件系统快照、备份或设备磨损副本。

正常验证：`ConversationDatabase.large_bodies_round_trip_once_and_survive_reopen`、`ConversationDatabase.idle_save_collects_bodies_the_record_no_longer_names`、`ConversationDatabase.erasing_the_conversation_removes_every_body`、`NativeAssistant.short_history_and_conversation_survive_clean_close_and_reopen`。新 SQL 路径未执行故障切断/恢复专项，不以现有 Core 历史证据代替。
