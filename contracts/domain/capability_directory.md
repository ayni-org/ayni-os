# Task 能力目录与按需授权

- **关联**：ADR-0085 决策 1、7、9；AP-03；ABI 0.50 / storage schema 6。
- **头文件**：`include/ayni/directory.h`、`ayni.h`、`calls.h`。
- **范围**：公开配置期说明采纳、Task 目录/决策租约和普通 Call 按需绑定。模型固定工具与标准消息由 Provider/harness 使用这些目录接口；不通过 Provider 回调 Core。上下文来源失效的处理见下文。

## 所有权与生命周期

backend/扩展产生候选；可信配置明确注册 Capability、Provider、Binding 后，才可调用 `ayni_register_capability_usage` 采纳确切配对的说明。目录不运行 backend、发现网络或 Provider 回调，不创建 Grant。catalog 拷贝摘要、用法和字段说明，在 start 后冻结；说明不持久化，也不是权限。未采纳说明的 Binding 仍可由可信策略经普通 Call 使用，但不出现在新目录中。

Goal 保存原工具策略的 mode、顺序和确切 Binding ID/version/role；恢复时当前 GoalType 策略失配形成兼容 hold，不重新解释原列表。REQUIRED=0 保持原必需 requirement 语义；ON_DEMAND=1 的允许列表只是可选上界，不要求先授予全部工具。子目标的可选列表须是每个可选祖先列表的子集；原必需 Scope/父绑定限制保持，列表不会替代父 requirement。

Task 保存已用工具的 `source_call_id` 和当前整份 Grant 引用；Call 保存首次选中的原 Grant、原业务证明及精确 Provider/endpoint/destination。原 Grant 的 Scope 是重新授权的上界，不另造授权账本。有效引用在 Task 可安全退役前保护原 Grant 和当前 Grant；UNKNOWN 和未释放借用仍阻止不安全回收。擦除正文不擦除这些授权/结果事实，也不重建正文。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-DIR-001 | 采纳只发生在可信配置期；摘要非空 UTF-8、至多 256 B，身份/用法总计至多 4096 B，字段说明按原域精确配对；完整拷贝后发布，重复/失配拒绝，不借用 backend 存储 | 发现即信任；说明建立 Grant；返回后源缓冲变化改写目录 | `CapabilityDirectory.copied_metadata_is_paginated_and_inspected_without_execution`；既有 `ToolUsage.copied_usage_survives_source_reuse_and_encodes_the_original_description` 与 `OpenaiUsage.invalid_description_is_rejected_during_trusted_adoption` 共用说明值校验 |
| C-DIR-002 | list/inspect 只披露原 Task 策略允许、元数据完整、当前有确切契约/路由的完整可用 Grant 链的 TOOL；不存在、未列举、缺说明及未授权查询均为 NOT_FOUND；调用另验实际参数、原需求与祖先，目录不是接受保证 | 缓存 inspect 结果绕过撤权；披露未授权工具名 | `CapabilityDirectory.unused_revocation_invalidates_cursor_and_hides_all_ungranted_entries`；`OnDemandBusinessCall.separate_grants_cannot_be_combined_for_one_optional_list` |
| C-DIR-003 | 目录按原策略顺序分页，最多 256 项且不超过部署 Binding 容量；单页 1..16 项；游标绑定 boot、Task、工作/持久前缀、待撤销 Gate 数量和最近有限 Grant 到期。可用 Task 的游标失配返回 REVISION_CONFLICT；当前 Task 授权拒绝优先。失败不发布部分输出 | 第九项不可达；旧游标跨 Task/重启继续；撤权后沿旧页输出 | `CapabilityDirectory.copied_metadata_is_paginated_and_inspected_without_execution`、`CapabilityDirectory.unused_revocation_invalidates_cursor_and_hides_all_ungranted_entries` |
| C-DIR-004 | snapshot 的 list/inspect 只在当前 decide owner 线程、call_context 租约内有效；普通 Provider、worker 和 decide 返回后不能通过回调查询 Core | worker 回调已结束的目录租约；普通 Provider 回调 Core 执行业务 | `CapabilityDirectory.directory_decision_callbacks_close_with_the_policy_lease`；调用链审查 |
| C-DIR-005 | 未授权可选工具不阻塞其他策略决策。首次调用由同一完整 Grant 覆盖全部规范列表项；与 Call 接受、Task 步骤/检查点同批保存原/现引用。未接受提议不绑定、不消费步骤；未用 Grant 撤销不暂停 Task，已用 Grant 撤销暂停该 Task，不自动替换 Grant | 列表逐项拼授权；未用工具缺权阻塞对话；同 Goal 其他 Task 被连带授权暂停 | `OnDemandBusinessCall.no_optional_grant_still_allows_a_normal_policy_decision`、`OnDemandBusinessCall.first_use_binds_one_whole_grant_with_its_source_call`、`OnDemandBusinessCall.separate_grants_cannot_be_combined_for_one_optional_list`、`OnDemandBusinessCall.initial_selection_tries_a_later_whole_grant`、`OnDemandBusinessCall.unused_revoke_does_not_pause_but_used_revoke_does`、`OnDemandBusinessCall.used_authority_is_owned_by_only_the_task_that_called` |
| C-DIR-006 | 显式 GOAL_REAUTHORIZE 原子作用于应用时全部 direct-live Task；原 Goal revision/authority seq 校验创建与授权代际，不是工具集合快照 CAS。每项新选择覆盖原 Grant 全 Scope/业务证明且保留原精确 Binding；保留 UNKNOWN、用户、隐私等独立 hold。管理查询可读 source/original/current 引用 | 较窄 Grant 只覆盖旧实际参数却解除暂停；换 Provider；复用旧工具集合 CAS 扩权；重新授权清除用户 hold | `OnDemandBusinessCall.explicit_reauthorization_covers_original_whole_scope_and_keeps_user_hold`、`TaskAuthority.optional_sources_survive_pause_reauthorization_and_portable_encoding`；精确 Binding/持久配对调用链审查 |
| C-DIR-007 | Goal 的冻结策略独立于注册输入寿命并随当前记录编码；子列表只可收窄祖先策略。已绑定来源数由 Task step capacity 限制，至多 64，不依赖目录页大小 | 新注册列表重写旧 Goal；子目标多列一个工具逃逸；目录分页改变授权引用 | `WorkRecords.goal_record_owns_more_than_eight_exact_optional_tool_references`、`CapabilityDirectory.child_optional_policy_can_narrow_but_cannot_expand_the_parent_list`、`ToolAllowList.registration_checks_each_reference` |

## 接口与资源

Owner C API 在 RUNNING 使用，非重入；管理引用查询是 owner 诊断入口。decide 内使用 snapshot 回调，不能重入公开 API。借用说明/Schema 可保留到 Kernel 销毁，但必须在每次 list/inspect/call 重新检查可用性。结构检查只收当前 minor 布局；cursor/info 是固定布局值类型，未来字段使用新类型。

每个 Binding 预留有界说明槽；每个 Goal/GoalType 拥有有界策略值，每 Task 的 live/staging 引用区随 Call/step 容量预留。记录预算按配置 Binding 容量推导；恢复记录中的策略数超过新配置容量为容量不足，保留旧目录。Native 内存数字见交付 PR，不外推 ESP32 栈/内存验收。

本包继续使用原模型业务工具传输，单响应仍至多 8 项；这不限制新目录容量。固定 list/inspect/call/shell、模型元数据外发、标准消息及预算压缩尚未由这一部分实现。新增格式按 ADR-0085 拒旧保留目录；本包没有启动新的故障/恢复、变异或平台专项，新增路径的故障证据仍未验证。

## 模型披露来源（ABI 0.50）

`capability_context` 捕获本 Task 冻结工具策略中当前可见的完整集合，并合并本次 decide 的目录读取；实际 Model 请求接受时复制 boot+256位来源。跨 Task 的外层 token 还保存 owner 与 exact GoalType/version。新增可见项不使旧来源失效，失去旧可见项或启动变化使之 EXPIRED。Model 接受、派发、晚结果与 snapshot 读取均复核；晚已知结果保留真实 outcome/effect/usage，但正文不可消费。harness 清除派生上下文并使用新的模型步骤，已接受业务操作事实仍由 Core 保留，不因重建重执行。

实际业务 Call 不继承 Model 的整份目录来源，只接受自己的原 Schema、单一完整 Grant 和稳定来源身份。目录/inspect 不是执行授权，撤销已用来源仍保持 AP-03A 的 Task 暂停与显式重新授权规则。
