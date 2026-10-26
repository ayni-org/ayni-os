# ADR-0054：类型化工具调用——允许列表 ∩ 授权、Provider 侧解码与 v4 调用行

- **状态**：已接受（实现 ADR-0042 决策 14、15；用户 2026-10-26 确认 P1 第一个 PR 先做 Core 侧接口）
- **日期**：2026-10-26
- **关联**：计划 §2.7、§3.3「新模型」、§9.1；ADR-0042、ADR-0045、ADR-0048、ADR-0053；MA-008C 前半；`contracts/domain/calls_and_bindings.md` C-CALL-014/015；`contracts/host/provider.md` C-PROV-003；`contracts/storage/records.md`「ABI 0.37」；[实施顺序](../../reports/core/m1-remaining-implementation.md)

## 背景

ADR-0042 决定 P1 先打通“用户文本 → 模型 → 工具调用 → 回答”，并定下两条：Provider 把模型输出转成 typed 结构，Core 与策略看不到原始 JSON（决策 14）；模型只能看到 GoalType 允许列表中该 Task 已获授权的部分，提出的每个调用仍走原有的 Binding、授权、资源与屏障校验（决策 15）。ABI 0.36 的调用结果只有 UTF-8 文本，Provider 也不知道有哪些工具可以描述给模型。

要在 Core 侧补齐的问题：

- 模型调用的结果怎样表示“最终文本或若干工具调用”，并能与调用行一同持久、重启后仍可读；
- Provider 在哪里、以什么生命周期拿到工具的字段类型、单位和取值，又不让 Core 为每次调用多备一块大缓冲；
- 列表外、授权外或越界的提议在哪一层被挡住，挡住后 Task 处于什么状态；
- 策略怎样读取提议、怎样执行其中一个，而不让“出现在提议中”变成一条绕开检查的路径。

## 决策

1. **新结果格式只属于模型。** `AYNI_CALL_RESULT_TOOL_CALLS`（2）只能用于非物理、非订阅的 MODEL 契约，其他契约声明它返回 `AYNI_INVALID_ARGUMENT`。这样的调用以 `COMPLETED` 结束时给出最终文本或 1～8 个类型化调用，二者择一；失败、取消、未知时与文本格式相同。一个类型化调用是精确 Binding（ID 与版本）、每个字段恰好一个值的参数（`ayni_scope_v1`）和 UTF-8 正文。
2. **GoalType 声明允许列表，启动时解析。** `ayni_goal_type_v1` 追加 `tools`：最多 8 个 `ayni_tool_ref_v1`（Binding ID 与版本），ID 不重复。注册只检查引用形状并复制；`ayni_start` 在全部 Binding 与执行映射校验之后把每一项解析为策略可以请求的 TOOL 类 Binding：与提交时同一项派发检查（非订阅；物理工具有执行映射，否则执行校验已先返回 `AYNI_NOT_FOUND`；非物理工具不占执行器资源）。找不到返回 `AYNI_NOT_FOUND`，是模型、订阅或无法派发的 Binding 返回 `AYNI_INVALID_ARGUMENT`，Kernel 留在 CONFIGURING，与策略链接相同。这样模型不会被提供一个策略照样提交也只会被拒绝的工具。列出不授予任何权限。
3. **提供给模型的工具 = 允许列表 ∩ 已授权。** 派发时，对允许列表中的每个 Binding，找 Task 所属 Goal 的第一项匹配需求：同 Capability 与版本、同来源域，且该需求的上限容许 Binding 自己的端点或目的地（执行目标/外发目的地字段）。找到的才提供，上限就是它的取值范围。判断只用不可变输入（启动时的目录与 Goal 创建时保存的需求），所以提交时与回执时得到相同结果。
4. **工具视图由回调填进 Provider 的存储。** `ayni_provider_request_v1` 追加 `tool_count`、`tool_context` 与 `tool(tool_context, index, out)`。Provider 在 `submit` 内按下标取视图 `ayni_tool_v1`：Binding 与 Capability 身份、该工具的输入正文上限、最多 8 个字段的用途、类型、单位与取值；路由字段只列 Binding 自己的端点或目的地。取值的 token 指针指向 `out` 自身，所以视图完整写在 Provider 提供的存储里。回调只在本次 `submit` 内、在其线程上有效；任何 `submit` 之外返回 `AYNI_INVALID_STATE`。Provider 不保留回调：在之后另一次 `submit` 期间它描述的是那次调用的工具，Core 不为每次 submit 另发令牌（Provider 是受信的 Host 组件）。Core 不为工具视图预留 arena。
5. **Provider 解码，Core 只收类型化列表。** `ayni_provider_result_v1` 追加 `tool_call_count` 与 `tool_calls`。Provider 用视图向模型描述工具，并把模型的工具调用解码为 typed 值；未知工具、无法解码的参数、视图外的值或超限正文，报告 `FAILED` 与新原因 `AYNI_REASON_MODEL_OUTPUT`（76），状态为 `INVALID_ARGUMENT`，列表超出结果上限时为 `CAPACITY_EXCEEDED`。策略看到的是一个普通的失败模型调用，可以重试或结束。
6. **post 时校验形状并就地编码。** `ayni_provider_post` 检查列表形状（数量 1～8、精确尺寸、目录 ID、每个字段一个值且不重复、UTF-8 正文、文本与列表不同时出现、只在 `COMPLETED`），错误返回 `AYNI_INVALID_ARGUMENT`，借用保持。通过后先计算编码长度：attempt 在派发时记下可接收的列表上限，即该调用的 `result_text_limit`（不大于结果通道）；超过返回 `AYNI_CAPACITY_EXCEEDED`，文本格式调用的 attempt 不接收列表，返回 `AYNI_INVALID_ARGUMENT`，借用都保持。否则 ProviderMailbox 在锁内经 `PayloadWriter` 直接把列表编码进终态通道；写入方先复制每个调用再复核，只写已测量的尺寸，Provider 若在测量后改动数组，只写入零，不越界，回执时按无效列表处理。post 返回后 Provider 可复用数组，Core 不另备缓冲。编码是“工具调用列表格式 1”（字段按 ID 升序，规范且无指针），与持久行使用同一份字节。
7. **回执时按视图复核，不合格是无效证据。** 应用终态时，Kernel 对每个调用重算提供结果：须对应一个已提供的 Binding（ID 与版本），参数被该需求上限覆盖（`authority::contains_request`），路由字段等于 Binding 的端点或目的地，正文不超过该工具的输入上限。任何一项不满足，调用以 `ABANDONED`/`RESULT_UNKNOWN`/`PROVIDER_RESULT` 结束，Task 按未知调用暂停（C-PROV-003）。Provider 已经拿到视图，仍发表视图外的内容是适配器缺陷，不是模型输出；不交给策略，避免策略把它当成可执行的决定。
8. **提议随调用行持久。** TOOL_CALLS 契约的每一行写 REQUIRED `CALL_STATE type21/v4`：v1 布局后接 u32 结果种类（1 文本、2 列表），种类 2 只能是 `COMPLETED`，列表放在原结果长度串中，受同一 `result_text_limit` 约束。旧 reader 遇到 v4 明确拒绝，不会把列表当文本读出。schema 2 不变，没有新记录类型，也没有跨记录配对。
9. **策略读取提议，自己决定执行。** 快照追加 `call_tool(context, step_key, index, out)`，只读本 Task 的保留调用，在 `decide` 内有效，把一个提议写成 `ayni_task_tool_call_v1`（字段与 token 存在 `out` 自身）。`ayni_task_call_result_v1.tool_call_count` 与 `ayni_call_info_v1.tool_call_count`（原 `reserved`）公开数量，`text` 为空，`ayni_copy_call_result` 复制 0 字节。Kernel 从不执行提议；策略要执行，就在下一次决定中以提议的 Binding、参数与正文提交 REQUEST_CALL，它与任何请求一样经过 Binding、授权、资源与派发屏障，提交时的授权状态才算数。
10. **Mock 提供确定性的工具调用。** `providers/mock` 增加 `tools` 服务（ADR-0048 的 A/B 变体同样适用）：输入开头的 `call:<binding_id>` 词各成为一个已提供工具的调用，字段取视图中的第一个值，其余文本为每个调用的正文；没有这类词时回显文本。未提供的工具、正文超出工具上限、列表超出结果上限都按决策 5 报 `MODEL_OUTPUT`。Mock 现在接受 Core 已检查过的类型化参数（不使用它们），以便作为工具被调用。

## 后果

- ABI 0.37：`ayni_goal_type_v1`、`ayni_task_snapshot_v1`、`ayni_provider_request_v1`、`ayni_provider_result_v1`、`ayni_task_call_result_v1`、`ayni_call_info_v1` 布局改变，新增 `ayni_tool_ref_v1`、`ayni_tool_v1`、`ayni_tool_call_v1`、`ayni_task_tool_call_v1`。主版本 0 只收当前布局（ADR-0053），Host、Provider、策略与 SDK 统一重编译。config 不变。
- 存储：schema 2 不变；新增 REQUIRED type21/v4。旧固件读取含 v4 的目录时受限并保留目录，与 type21/v2、v3 相同，本轮无迁移工具。
- 内存（`tools/measure_kernel_memory.c`，Native 64 位 GCC ASan 构建，对齐均为 8 B）：默认 216,776 → 219,672 B（+2,896），最小工作 12,360 → 12,736 B（+376），双 Agent 调用 482,552 → 484,392 B（+1,840），双 Agent 执行 535,576 → 537,416 B（+1,840）。几乎全部来自每个 GoalType 槽内固定的 8 项工具引用（每槽 360 B）；调用槽、ProviderMailbox 结果和 Delivery 中的结果种类与工具位图在各配置共计 16～40 B。attempt 的列表上限落在已有对齐填充内，不改变上述数字。
- 每个 Provider 的 `ayni_provider_result_v1` 初始化都要写新字段；现有 Mock、Mock 执行端与测试已更新。下一个 PR 的 OpenAI-compatible Provider 用 `tool` 回调生成 function 声明，在 Provider 内完成 JSON 参数解码与视图比对。
- 回执时的复核重新读取 Goal 的需求与目录，代价与 `check_input` 同级，每个调用最多 8 次；不额外保存提交时的视图。
- post 在 ProviderMailbox 锁内完成列表校验与编码，代价受 8 个调用与该调用的结果上限约束；回执时再解码一次。两者都是有界的线性扫描，不值得为此保存解码结果。
- 不合格列表让 Task 按未知调用暂停，需要所有者处理；这只在 Provider 适配器有缺陷时发生，正常的模型错误走决策 5 的 `FAILED`。

## 备选方案

- **把 Capability 的完整取值域交给模型**：模型会提议 Goal 上限之外的值，策略再请求时才被拒绝，浪费一次模型调用，也把 Task 不能用的取值暴露给外部服务。按 Goal 上限裁剪与决策 15 一致。
- **Kernel 自动执行模型的提议**：策略失去决定权，组合与停止条件无处表达；“出现在提议中”会变成一条与普通请求不同的执行路径。决策 15 要求每个调用仍走原检查，由策略提交是最直接的保证。
- **Core 在 arena 里为工具视图预留数组**：每次派发要能容纳 8 个工具 × 8 个字段 × 8 个 token 的视图结构，而只有模型派发用到；回调把存储交给已经要保存这些信息的 Provider。
- **owner 在 consume 时才编码，Provider 保留数组到那时**：Provider 不知道 owner 何时消费，只能把数组生命周期延长到下一次确认，与“post 后借用结束”的现有规则不同。在 post 内编码保持了现有规则。
- **回执时把视图外的提议记为 `FAILED`/`MODEL_OUTPUT`**：Core 分不清模型错误与适配器缺陷；把后者交给策略，策略可能据此重试同一个错误的适配器。按 C-PROV-003 记为未知并暂停更保守。
- **列表单独成为一种记录**：需要新的记录类型、同批配对与快照顺序；放在原结果长度串中只需一个尾部字段和一个版本号。
- **Core 接收模型 JSON 再解码**：ADR-0042 决策 14 已否决：Core 不引入 JSON 解析，厂商格式差异留在 Provider。

## 验证

- 编码：`ToolCallsTest.round_trip_keeps_typed_values_in_canonical_field_order`、`ToolCallsTest.provider_shape_errors_are_classified_and_measure_nothing`、`ToolCallsTest.decoder_rejects_noncanonical_or_truncated_lists`。
- 契约与记录：`CallContract.tool_call_results_belong_to_nonphysical_unary_or_action_models`、`CallTableTest.tool_call_list_result_is_retained_and_round_trips_as_v4`、`CallTableTest.tool_call_list_needs_its_contract_and_fits_its_bound`、`ProviderMailboxTest.written_terminal_keeps_its_kind_and_lane_bound`（列表超出该调用上限、文本调用收到列表）。
- 公开接口端到端：`ToolCalls.model_call_is_offered_only_listed_tools_within_the_goal_ceiling`、`ToolCalls.strategy_requests_a_proposed_call_through_the_ordinary_checks`、`ToolCalls.malformed_lists_are_refused_on_post_and_keep_the_borrow`、`ToolCalls.unoffered_tools_and_values_outside_the_offer_end_unknown`、`ToolCalls.proposed_calls_survive_restart_with_the_call_row`。
- 允许列表：`ToolAllowList.registration_checks_each_reference`、`ToolStart.start_needs_each_tool_to_be_an_ordinary_tool_binding`（未知 ID、错版本、模型、订阅；物理工具缺执行映射由执行校验先行拒绝，非物理工具占执行器资源与提交时共用 `Catalogs::dispatchable`）。
- Mock：`MockTools.proposes_the_offered_tool_at_its_first_values_and_the_proposal_runs`、`MockTools.input_without_call_words_is_echoed_as_text`、`MockTools.unoffered_tool_fails_as_model_output`、`MockTools.body_over_the_tool_limit_fails_as_model_output`、`MockTools.list_over_the_result_limit_fails_as_model_output`。
- ABI 布局：`tests/contract/abi_layout.c` 覆盖新增与变化的结构体。
- 未做：新的恢复/故障专项（重启只验证了正常的 v4 往返），以及真实模型服务的端到端，后者随 P1 第二个 PR。
