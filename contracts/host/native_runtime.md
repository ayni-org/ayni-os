# Native 双 Agent 应用运行时

- **关联**：ADR-0057、ADR-0060；MA-020 前半；计划 §9.1。

条款描述本机可信演示装配，不构成远程管理认证或硬件支持。

| 编号 | 规范 | 反例 | 验证 |
|---|---|---|---|
| C-NATIVE-001 | A 预设规则、B ToolLoop 通过公开意图与独立实例运行；缺授权保存待批；正常重开继续原待批 Goal，不重复创建 | CLI 从配置直接给策略绕过 Grant，或重开另建同一工作 | M1 已实现：`NativeAssistant.rule_and_chat_agents_use_separate_owned_work`、`NativeAssistant.proposed_turn_can_be_approved_after_normal_reopen`、`NativeAssistant.rule_agent_restores_its_pending_goal_and_reports_completion` |
| C-NATIVE-002 | 对话只由完成且 durable 的 Task 检查点产生候选，异步文件必须匹配 Context ID/revision/generation/轮次，精确 ACK 持久后才展示；等待完成包含文件提交与 ACK，正常关闭重开保留正文与摘要 | Task 刚内存完成即宣布已保存，下一轮回收其检查点 | M1 已实现：`NativeAssistant.summary_and_conversation_survive_clean_close_and_reopen`、`NativeAssistant.continuing_past_retained_capacity_keeps_latest_context_and_queryable_calls`；异常退出窗口及文件故障未验证 |
| C-NATIVE-003 | 整段/单轮删除走 Kernel 屏障与外层 ACK，清理受管理历史正文并保留实际动作结果；完成定义见 C-MEM-008～011 | 把外层值清空宣称全盘擦除 | M1 已实现：`NativeAssistant.whole_context_delete_erases_content_and_preserves_actual_work_outcomes`、`NativeAssistant.deleting_one_turn_preserves_other_users_and_rebuilds_summary_from_valid_inputs`；新故障/恢复专项未验证 |
| C-NATIVE-004 | Native 文本摘要声明 TEXT，不向摘要提供工具；模型与工具仍经过原授权、资源与持久屏障 | 摘要被模型提议转成业务工具执行 | M1 已实现：`NativeAssistant.local_http_tool_loop_and_text_only_summary_use_real_provider_wiring`；仅本地 HTTP 夹具，实 API 多轮未验证 |
| C-NATIVE-005 | Native/Sim 复用同一 Profile；Sim 时钟/日志相互独立，正常释放 Kernel 后重新装配保留日志 | 两个模拟节点共用可变 Host，重开丢失 boot 历史 | M1 已实现：`SimDemo.independent_nodes_use_the_same_profile_and_retain_storage_on_release` |
| C-NATIVE-006 | 管理操作通过公开意图提交，等待完成须查询 receipt 到 DURABLE，包含 PENDING 且 commit_seq 为零的撤销；客户端容量满则提交前拒绝 | 把接受命令当成已持久，或漏掉 PENDING 回执 | M1 已实现：`NativeAssistant.grant_revoke_and_cancel_use_public_management_commands`（正常提交与持久等待）；PENDING 背压的 runtime 专项未验证，Core 既有回执测试保持 |
