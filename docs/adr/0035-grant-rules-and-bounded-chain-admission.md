# ADR-0035：Grant 规则归属与有界父链准入

- **状态**：实施中；独立审查、变异与完整本地验证尚未结束。
- **日期**：2026-10-23
- **关联**：`MA-006` R2b1；[详细计划](../ma006-implementation.md) §3.2；[复查清单](../../reports/core/ma006-a-d-review.md) A3、C2、C4～C7、D3；ADR-0030/0031/0034；C-AUTH-019～024、026/027。

## 背景

Grant 的结构体、记录与命令分别重复标志和期限规则。创建时父链在 GrantTable 与 WorkModel 中反复遍历；主体可用性也有四份判断。撤销根、后继 ID、终态更新和恢复祖先检查存在多份实现。缺少注释和混合的恢复函数，使错误顺序、日志与快照的不同证据难以核对。

本 PR 整理这些既有机制，不改变公开行为、ABI 或持久格式。命令暂存/回执/重放配对留给 R2b2，Kernel step/快照编排留给 R2b3；各份分别通过完整流程。

## 决策

1. **规则留在不变量属主。** `authority/grant/grant_rules.h` 拥有请求已知标志、未实现特性标志、持久允许标志及期限/时长关系。入口仍保留自己的检查次序：未知位是非法输入，已知 Scene/Quota 是明确不支持，存储只能有已支持标志。`AgentTable::usable()` 拥有主体可用性，保持 PAUSED 可作为授权主体、STOPPED/FAULTED 不可用。Kernel 的同一管理端检查供创建和撤销复用，不将会话身份规则下移到 GrantTable。
2. **父链只定位一次，工作主体仍由 Kernel 检查。** GrantTable 的成功 `check_chain()` 发布局部 `CheckedChain`，最多保存最大委派边数加根的索引；失败不改输出。`Admission` 带父链，根创建使用明确的空链。WorkModel 在整个授权链通过后，按这些索引检查主体，不再次遍历或重复授权检查，保留授权/容量错误优先于祖先主体错误的顺序。该值只在单次所有者线程调用内消费；期间禁止表变动、回调或重入，不是跨调用缓存。
3. **共用有界遍历，保留不同的边界含义。** GrantTable 私有 `visit_ancestors()` 只接受内部判定，返回到根、判定停止或断链/超限。完整链和创建历史检查允许最大深度加一个根节点；后代边关系与撤销 frontier 检查允许最大深度。创建历史虽从父开始，仍保留 max+1 预算，以保持降低配置深度时先判有效历史、再返回容量不足的既有错误分类。`descendant_of()` 仍先比较父 ID 再查父槽，完整链仍先验证边再检查父状态。`history_at_creation()` 返回 active/later_terminal/invalid，代替用布尔参数选择日志或快照算法。
4. **父边共同契约与可用证据分开。** `check_parent_contract()` 集中所有者、绑定 revision、exact capability/version、完整源域、再委派许可和期限种类。实时创建比较 deadline；保留事实校验创建 boot、duration 及存在的 deadline；恢复仅有持久 boot/duration。三者共用共同规则，但不假造不存在的时间证据。`no_grant_deadline_us` 明确永久授权或恢复限时授权没有活 deadline，后者不可继续运行。
5. **撤销与回收只有一份选择/更新规则。** `is_revocation_root()` 只比较已有效事实的 root identity，调用方仍检查需要的生命周期。`next_id_after()` 在冻结的 inclusive frontier 内按 ID 选后继，失败不改输出；实时清理与恢复使用同一规则。`revoke_by()` 统一首次终态更新，根的 cleanup stamp 独立推进，不改变终态回收顺序。字节区定位使用唯一 stride 和具名 `grant_value_regions`。
6. **恢复分为检查、放置与发布。** `RestoredState` 将输入的生命周期/revision/两个 stamp 组合；共享事实校验和最终复制。日志创建要求祖先仍活跃并消费记录指定的精确替代对象；快照允许创建后才终止的祖先，要求 predecessor 已不存在，并按父先子后追加。两路分别调用具名步骤，不再传布尔模式。所有可失败检查在发布前完成；损坏与容量不足的分类顺序保持。
7. **会话与兼容。** 公共入口把已查到的 Principal 传给 `open_session()`；Session.binding_revision 明确是打开时捕获的身份快照，后续不能随主体变更自动升级。没有新增 Kernel 字段、持久缓存、堆分配、I/O 或外部回调。ABI 保持 0.19，记录及 arena 布局不变；CheckedChain 是最多 17 个索引加数量的局部值，其实际大小与两组 arena 需求在验证中实测。既有测试断言保持，逐测试补契约引用；机器检查仅移除已消除的四项历史基线。

## 后果

- 更改主体状态准入只需改 AgentTable 规则；更改 Grant 标志只改 authority 规则及其入口错误顺序测试；更换 Host 不改变这些 portable Core 规则。
- private visitor 的新增抽象服务五种已有祖先遍历，维护一份循环/断链逻辑；不暴露回调 API，也不创建通用树框架。
- Grant.revision 仍是生命周期对应的持久公开事实，保留字段并明确 ACTIVE/终态的不变量。把它删除会改变表/记录消费者，本次不做。
- `CheckedChain` 增加少量有界临时栈，换取不重复父 ID 查找和授权检查；最大深度的内存大小实测不等于最坏 step 栈水位或时长测试。
- 本次不实现 Goal 审批、活工作引用或授权恢复；后续仍按 R2b2→R2b3→R3→R4→E→F 交付。

## 备选方案

- 保留每个调用者自己的规则与树遍历：无法保证错误优先顺序及边界同步，拒绝。
- GrantTable 直接访问 AgentTable：会使 authority 依赖 work 并混淆不变量属主；改为返回短期索引路径。
- 持久缓存完整祖先链：引入替代/恢复失效维护与 arena 占用；本次只用同一次调用中的局部结果。
- 合并日志与快照放置：两者对终态祖先和 predecessor 的契约不同，保留两个具名入口。

## 验证

原始命令、源码身份、变异清单、运行日志及覆盖率位于仓库外 `../ayni-ma006-evidence/r2b1/`。初版 GCC Debug 完整 CTest 907/907 通过。独立会话 `/root/r2b1_review` 在阅读测试前依据生产 diff/契约冻结 25 项变异（SHA-256 `f9f3f94397539539f406147419d64ab17e162b7f3852c29c8cd85af653a51f31`）；25 项均可编译并运行完整原 892 项单元，首轮 19 检出/6 存活。补六项行为测试后，原六项变异全部检出，没有等价或编译失败排除。两轮源码首尾 SHA 相同，正常还原分别 892/892、898/898 全过。最终五配置、覆盖率和两组 arena 内存已验证，结果如下；ESP-IDF 固件编译尚待完成。

### 作者逐项可读性自查

1. **删除注释。** 检查全部生产 diff 的删除注释，只有 `work_model.h` 旧内部 `open_session` 对未知主体返回 NOT_FOUND 的说明不再适用：它现在接收已解析 Principal。公共入口 `work_api.cpp:332` 仍先查主体并保留 NOT_FOUND；新内部前置条件见 `work_model.h:107`。原有创建、替代、恢复注释仍保留，并补充到 `grant_table.cpp:530`、`:735`、`:789`、`:899` 的属主步骤。
2. **Kernel 新字段。** 检查全部 `core/kernel/` diff 与未改动的 `kernel_state.h`，没有新 Kernel 字段。`grant_table.h:136` 的 CheckedChain、`:151` 的 Admission 路径、`:275` 的 RestoredState 都是单次调用的局部结果；前者借用不变的 GrantTable 索引，不是保留缓存。WorkModel 只消费路径并负责 work 主体，不复制 authority 状态。
3. **重复公式。** 搜索 flags/lifetime、usable、parent/grant ID 遍历、revoked_by_root_id 比较与 domain stride。标量规则归 `grant_rules.h`，主体规则归 `agent_table.h:92`，管理端规则归 `authority_intent.cpp:7`；`grant_table.cpp:134`、`:174`、`:376` 和 `grant_table.h:242` 分别拥有地址、后继 ID、父共同契约和祖先遍历。记录 decoder 也复用根身份判断。实时 deadline 与持久 boot/duration 不合并为假通用算法，理由见决策 4。
4. **裸数字。** 检查生产 diff 及 Grant 全模块的索引、移位、生命周期和字节计算；双字节区用 `grant_value_regions`，无 live deadline 用 `no_grant_deadline_us`，revision 沿用唯一具名值，Kernel ID 只经 foundation 解析。max+1 的根节点预算及历史恢复分类在 `grant_table.h:236` 明确；记录布局值继续来自记录属主。无新增公开状态码。机械基线 34→30，无新增违规，不能替代人工审查。

### 独立审查

独立会话先检查属主/依赖、调用者、失败顺序和恢复消费者，再逐项检查四项可读性。发现两项 P3：state decoder 尚有一处根身份直接比较，以及祖先遍历注释没准确说明创建历史的 max+1 预算；均已修复并复核关闭。未发现可证明的行为/持久字节回归。六个新增测试的静态复核通过，原断言未删改；审查者另核对首轮 XML、命名失败与源码还原，实际构建由实现者运行。最终验证证据在执行后追加。

### 独立变异逐项结果

首轮结果与补测分开保留。下表首轮检出列一项实际失败测试；存活项列后加的测试，均重跑原选点检出。

| 编号 | 文件与植入变化 | 首轮 | 首轮失败 / 补测 |
| --- | --- | --- | --- |
| R2B1-01 | `core/authority/grant/grant_rules.h`：把只允许 delegation 的已接受事实放宽为允许 Scene/Quota；检验请求可识别与持久事实可执行的区别。 | 检出 | `CommandRecords.v1_auth_missing_identity_unsupported_flags_and_wrong_results_are_never_written` |
| R2B1-02 | `core/authority/grant/grant_rules.h`：CURRENT_BOOT 接受零 duration，破坏期限表示和字节/结构体共用验证。 | 检出 | `CommandCanonical.invalid_identity_or_structure_never_publishes_a_size_or_bytes` |
| R2B1-03 | `core/authority/grant/grant_table.cpp`：把 recognized-but-unsupported 检查移到 malformed scalar 之前；复合错误应维持 INVALID_ARGUMENT 优先级。 | 存活；补测检出 | `GrantTable.malformed_scalars_precede_deferred_features_without_publishing_admission` |
| R2B1-04 | `core/authority/grant/grant_table.cpp`：把恰好到 UINT64_MAX 的合法 deadline 当溢出拒绝，检查加法边界。 | 检出 | `GrantTable.malformed_flags_lifetimes_revisions_and_overflow_never_publish_a_plan` |
| R2B1-05 | `core/authority/grant/grant_table.cpp`：完整链少给一个节点预算，合法最大边数的链遗漏根并被拒绝。 | 检出 | `GrantTable.every_ancestor_hold_and_the_edge_depth_limit_apply_to_descendants` |
| R2B1-06 | `core/authority/grant/grant_table.cpp`：交换 Gate/REVOKED 与易失 hold 优先顺序，让同时受拒绝和目录不兼容的链报告 hold。 | 存活；补测检出 | `GrantTable.ancestor_gate_and_terminal_revocation_precede_its_existing_hold` |
| R2B1-07 | `core/authority/grant/grant_table.cpp`：漏校验 leaf 到直接 parent 的边，仍访问祖先但无法证明完整链收窄。 | 检出 | `GrantTable.chain_checks_fail_closed_when_retained_facts_or_ancestor_links_are_corrupt` |
| R2B1-08 | `core/kernel/intent/work_model.cpp`：消费 CheckedChain 时省略最后一个根主体，允许不可用根的不同主体后代通过。 | 检出 | `GrantApi.shared_kernel_check_denies_missing_or_newly_faulted_subject_facts` |
| R2B1-09 | `core/authority/grant/grant_table.cpp`：共享父契约漏掉再委派许可，live/retained/replay 均可衍生不可委派父授权。 | 检出 | `GrantTable.chain_checks_fail_closed_when_retained_facts_or_ancestor_links_are_corrupt` |
| R2B1-10 | `core/authority/grant/grant_table.cpp`：共享父契约允许限时父产生永久 child，检查期限类型而非仅绝对 deadline。 | 检出 | `GrantTable.chain_checks_fail_closed_when_retained_facts_or_ancestor_links_are_corrupt` |
| R2B1-11 | `core/authority/grant/grant_table.cpp`：拒绝与 parent 同 deadline 的合法收窄 child。 | 检出 | `GrantTable.finite_child_deadline_cannot_outlive_its_parent_remaining_time` |
| R2B1-12 | `core/authority/grant/grant_table.cpp`：允许 child 与祖先同 creation stamp，破坏严格创建先后历史。 | 存活；补测检出 | `GrantTable.equal_parent_creation_stamp_is_rejected_by_journal_and_snapshot` |
| R2B1-13 | `core/authority/grant/grant_table.cpp`：journal create 错用 snapshot 历史规则，允许当前表已终态的祖先参与创建重放。 | 存活；补测检出 | `GrantTable.only_snapshot_accepts_child_history_beneath_a_later_terminal_parent` |
| R2B1-14 | `core/authority/grant/grant_table.cpp`：snapshot 错用 journal 规则，拒绝 child 创建后才终态的合法祖先历史。 | 检出 | `GrantReplay.snapshot_preserves_child_mapping_when_parent_later_expires_and_changes_revision` |
| R2B1-15 | `core/authority/grant/grant_table.cpp`：清理选择最大而非最小后继 ID，跨过未扫描对象并破坏一致的 live/replay 次序。 | 检出 | `GrantRevoke.cleanup_finishes_at_frozen_frontier_when_a_retained_unrelated_leaf_was_replaced` |
| R2B1-16 | `core/authority/grant/grant_table.cpp`：错误排除冻结 through 端点；末端 ACTIVE 后代可能只推进水位而未撤销。 | 检出 | `GrantRevoke.durable_revoked_child_is_reclaimed_before_root_and_progress_does_not_reorder_root` |
| R2B1-17 | `core/authority/grant/grant_table.cpp`：终态持久但最近 cleanup 尚未持久时提前回收。 | 检出 | `GrantRevoke.incomplete_cursor_gate_and_unconfirmed_progress_each_block_leaf_reclaim` |
| R2B1-18 | `core/authority/grant/grant_table.cpp`：有 Gate/pending 引用的终态叶节点可以让位，损坏已接受控制的保留边界。 | 检出 | `GrantRevoke.incomplete_cursor_gate_and_unconfirmed_progress_each_block_leaf_reclaim` |
| R2B1-19 | `core/authority/grant/grant_table.cpp`：共享终态更新保留 ACTIVE revision，破坏 lifecycle/revision 同步不变量。 | 检出 | `GrantRevoke.descendant_snapshots_reject_private_cursor_fields_and_out_of_order_stamps` |
| R2B1-20 | `core/authority/grant/grant_table.cpp`：进度更新覆盖根第一次终态 stamp，改变安全回收顺序与已保留 CHANGED 结果身份。 | 检出 | `GrantRevoke.cleanup_finishes_at_frozen_frontier_when_a_retained_unrelated_leaf_was_replaced` |
| R2B1-21 | `core/authority/grant/grant_table.cpp`：journal 的显式 replaced ID 可回收仍有保留 child 的祖先，违反精确替换的安全前提。 | 存活；补测检出 | `GrantTable.replay_cannot_replace_a_terminal_parent_with_active_or_terminal_retained_child` |
| R2B1-22 | `core/authority/grant/grant_table.cpp`：恢复安装把 duration 当成可用 deadline，复活同 boot 恢复的限时授权。 | 检出 | `GrantApi.restart_keeps_permanent_grants_and_expires_finite_grants_without_reanchoring` |
| R2B1-23 | `core/work/agent/agent_table.h`：把 PAUSED 错判不可用并让 STOPPED 通过，检验共享主体可用性的生命周期边界。 | 检出 | `GrantApi.a_stopped_or_unregistered_subject_cannot_receive_a_new_grant` |
| R2B1-24 | `core/kernel/intent/authority_intent.cpp`：管理会话不再匹配身份快照 revision，允许旧绑定 Session 取得新授权或撤销。 | 检出 | `GrantApi.kernel_semantic_boundary_rejects_unproven_or_stale_owner_evidence` |
| R2B1-25 | `core/kernel/intent/work_model.cpp`：满 Session 表失败时先消耗 ID，破坏准入失败不改变分配状态的边界。 | 存活；补测检出 | `WorkModel.a_full_session_table_refusal_does_not_consume_the_next_identity` |

### 本地构建、覆盖率与资源

- GCC 13.3 Debug、Release、ASan/UBSan、TSan，Clang 18.1.3 Debug 各 913/913 CTest（898 单元 + 15 契约/工具）；warnings-as-errors。模拟器、83 项 Python 工具测试、依赖/契约/可读性、全文件 clang-format 18、diff 检查通过。
- TSan 临时使用 `vm.mmap_rnd_bits=28`，完成后核对已恢复原值 32。没有运行远端 CI；依据用户本轮本地验证授权，不修改 CI 配置。
- 首轮变异容器 2 CPU/3 GiB，实测峰值约 632 MiB；确认宿主约 14 GiB 可用、无持续换页/内存压力后，补测和最终 native 使用 4 CPU/3 GiB、构建 4 jobs、CTest 2 jobs。补测峰值约 688 MiB；覆盖率另用 2 CPU/3 GiB。并行前按合计最多 6 CPU/6 GiB 预留预算，cgroup 无 OOM；保留资源日志。首个普通用户构建预检曾因旧 root 构建缓存写权限失败，尚未植入变异；修正仅本项目构建目录属主后开始正式首轮，失败日志单列保存。
- GCC 13 gcov 构建后清空 gcda，完整运行 913/913 CTest；收集全部翻译单元及头文件消费者。分支按 ADR-0034 修正后的 throw 过滤/插桩布局分组方法计数，不排除防御分支；改动基线 main `84b3a8f`，源码首尾 SHA 相同。

| 范围 | 行覆盖 | 分支覆盖 |
| --- | --- | --- |
| Core（含头文件） | 7717/7894，97.76% | 5525/6080，90.87% |
| Grant/主体准入属主组件（含头文件） | 1099/1110，99.01% | 1019/1080，94.35% |
| 本轮改动可执行行 | 266/268，99.25% | 297/312，95.19% |

未命中的分支保留在原始清单：恢复时无 deadline 的防御组合、Scope/深度容量分类、已完整验证候选后不存在 next ID 的防御路径、损坏链耗尽节点预算，以及 journal/snapshot 中根身份条件的部分短路。新增 Kernel 集成行单独为 15/18 分支（83.33%），三项未命中属于 journal/snapshot 根身份短路；不把该局部数字隐藏在总数里。本轮属主与改动的安全分支整体超过 90% 目标。

C99 探针实测默认 **110,792 B**、最小工作容量 **31,616 B**，对齐 8，与 ADR-0034 不变。该数字是 caller arena 需求，不是运行时 RSS、栈水位或硬件内存验收。
