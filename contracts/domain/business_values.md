# 通用业务值与授权投影

- **关联**：AP-02、ADR-0084；ABI 0.48；[调用契约](calls_and_bindings.md)、[记录格式](../storage/records.md)。
- **属主**：catalog 拥有静态 Schema/投影；authority 拥有完整授权候选；work 拥有规范请求和结果；适配器只处理语法及副本。

Schema/value 是有界 preorder 表。支持 OBJECT、LIST、BOOL、I64、U64、有限 binary64、固定 scale/I64 coefficient 的 DECIMAL、ENUM、IDENTIFIER、TEXT，以及显式 nullable。对象字段 ID 稳定，JSON 名字来自原 Schema；Core 不解析 JSON。默认值为注册时复制的静态定义，在授权前只填一次。运行时来源正文与规范值属于可擦除内容，静态 Schema/投影和原身份保留不能重建已擦除正文。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VALUE-001 | 原生值与具名源共用 Schema 规范化；未知/重复字段、缺失必需值、非法 NULL、类型/范围/深度/容量错误整次拒绝；失败不部分发布 | 模型参数先按一套规则批准，再由 Provider 用另一套规则执行 | `BusinessValues.malformed_tree_and_unknown_fields_are_rejected`、`BusinessArguments.named_fields_share_native_defaults_and_reject_duplicate_keys`、`BusinessValues.small_output_reports_requirements_without_touching_buffers` |
| C-VALUE-002 | 对象按稳定 ID 排序并补默认；列表保留顺序及重复；近似浮点只接受有限值并规范 -0，保留合法 subnormal | 排序去重列表改变业务效果；拒绝所有 subnormal | `BusinessValues.object_fields_sort_by_id_and_static_defaults_fill_missing_values`、`BusinessValues.lists_preserve_order_duplicates_and_canonicalize_negative_zero`、`BusinessArguments.approximate_numbers_round_once_and_keep_legal_subnormals` |
| C-VALUE-003 | DECIMAL 使用 exact coefficient/scale，不舍入、不经 double；JSON DECIMAL 使用十进制 STRING；宽整数 Schema 的所有整数取值使用 STRING，approximate FLOAT64 以 NUMBER 表示 | UINT64_MAX 经 double 损失精度；把 0.1 的近似表示当权限事实 | `BusinessValues.decimal_keeps_scale_and_signed_extremes_without_rounding`、`BusinessArguments.wide_integers_and_exact_decimals_never_pass_through_binary64`、`BusinessValues.json_sdk_keeps_decimal_extremes_and_explicit_capacity_failure` |
| C-VALUE-004 | 原授权域和完整业务/执行证明分开保存；Schema、默认值、单位、scale、投影及执行属性必须 exact 匹配，历史 Grant/Goal/Call 不由当前目录重释 | 尚未执行的旧 Grant 被新 Schema 重新解释 | `BusinessSchema.canonical_proof_sorts_declarations_and_retains_exact_decimal_scale`、`BusinessContract.complete_proof_round_trips_empty_and_nonempty_projection_sets`、`GrantTable.original_business_proof_is_copied_and_catalog_changes_hold_unused_grants` |
| C-VALUE-005 | 投影只读规范请求的固定路径，至多一个共同非空列表轴，全部行由同一完整 requirement/Grant 覆盖；可继续尝试后一个完整候选 | 第一个 Grant 批准前半行、第二个批准后半行后拼接执行 | `BusinessCall.a_list_split_across_two_saved_grants_is_denied_without_dispatch`、`BusinessCall.a_later_complete_requirement_can_cover_the_whole_list`、`BusinessProjection.exact_decimal_rows_keep_one_route_and_copy_token_storage_safely` |
| C-VALUE-006 | submit/post 边界复制有界值及所有活动字符串，回调视图不延长借用；owner 按 Call 原结果 Schema 校验和补默认后才发布 | worker 保留 Core 节点指针；非法结果让 Goal 完成 | `BusinessCall.copied_business_values_and_result_defaults_complete_the_same_task`、`BusinessCall.schema_invalid_structured_terminal_does_not_complete_the_goal` |
| C-VALUE-007 | 已失效结构正文不再可消费，原身份/outcome/effect 保留，独立于隐私擦除；恢复不得补默认或重派发 | 清空 body 却保存成合法空 VALUE，让策略消费或恢复失败 | `BusinessCall.unavailable_structured_body_keeps_a_valid_original_call_fact` |
| C-VALUE-008 | 适配器/策略只编解码原 Schema；不增加逐能力参数分支，不把文本打印结果作为执行参数；容量不足明确报错 | ToolLoop 发明 field_17 参数名；JSON fixture 只能调用硬编码能力 | `StructuredToolLoop.nested_input_and_structured_result_complete_the_same_conversation`、`NestedMockTools.named_json_keeps_nested_list_order_duplicates_and_exact_decimals`、`NativeAssistant.model_schedule_tool_registers_a_goal_that_waits_for_owner_approval` |

权限不使用浮点、自由文本、任意表达式或隐式单位转换。BOOL 只能显式映射为 0/1；scale=0 decimal 保持相同单位，非零 scale 使用显式 coefficient 单位。关联、跨列表、总量策略不在首版。软件 Shell 的脚本是它显式 TEXT 业务字段，宽 OS 执行权由部署授权；它不是从文本推导低权限的机制。

Provider 收到 SUCCESS post 返回值只表示可靠 mailbox 已接收并归还借用。语法有效但违反原业务 Schema 的终态/工具证据由 owner 记为未知，不能由 producer 再发第二个结果补救。C/C++ helper 的纯读取不授予权限；独立线程只可检查自己复制且保持不变的 proof/value。

上述是本包相关正常/边界验证范围；旧有恢复测试继续回归，不表示新增故障/恢复、平台、硬件或付费模型专项通过。
