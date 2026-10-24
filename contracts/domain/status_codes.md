# 状态码

- **关联**：计划 §3.2「错误」、§3.4；ADR-0006、ADR-0017、ADR-0018、ADR-0020、ADR-0022、ADR-0026、ADR-0027、ADR-0030；`MA-004`、`MA-005`、`MA-010`
- **定义位置**：`include/ayni/ayni.h`（唯一来源；Core 内部不复制一份）

## 取值

0 表示成功，负数表示失败。已发布的值不改作他用；调用方把不认识的负值当作一般失败处理。状态码描述的是**这一次调用**的结果，不是用户文案，也不是领域对象的状态。

| 值 | 名称 | 含义 | 调用方可以做什么 |
|---:|---|---|---|
| 0 | `AYNI_OK` | 调用完成了它声明的效果 | — |
| -1 | `AYNI_INVALID_ARGUMENT` | 参数本身不合法：空指针、长度或对齐不符、缺少必需字段 | 修正调用；重试同一参数仍会失败 |
| -2 | `AYNI_INVALID_STATE` | 参数合法，但对象当前状态不允许该操作 | 等待状态变化后再试 |
| -3 | `AYNI_UNSUPPORTED_VERSION` | ABI、协议、存储或能力版本不受支持 | 升级或降级一方 |
| -4 | `AYNI_UNSUPPORTED` | 本构建或本设备不提供该功能（显式拒绝，不静默降级） | 改用其他能力或配置 |
| -5 | `AYNI_NOT_READY` | Kernel 不在 `RUNNING`，或依赖尚未就绪 | 等待生命周期事件后重试 |
| -6 | `AYNI_BUSY` | 暂时没有容量（如完成槽已满），形成背压 | 稍后重试同一请求 |
| -7 | `AYNI_CAPACITY_EXCEEDED` | 达到配置的容量上限或内存不足 | 释放资源或调整配置；不会自动恢复 |
| -8 | `AYNI_BUFFER_TOO_SMALL` | 调用方缓冲不足；需要的长度已写入输出参数（如有） | 按所需长度重试 |
| -9 | `AYNI_NOT_FOUND` | 引用的对象不存在于本机目录或记录中 | 不能据此推断外部动作未执行（C-STATUS-005） |
| -10 | `AYNI_DENIED` | 授权、主体或场景检查拒绝 | 取得授权；不应换个参数绕过 |
| -11 | `AYNI_EXPIRED` | 期限、租期、许可或去重低水位已过 | 重新准入或以新身份发起（遵守 C-CALL-008） |
| -12 | `AYNI_STALE` | 新鲜度证明（`receiver_tick`、观察有效期、依赖版本）不足 | 取得新证明后以同一幂等键重试 |
| -13 | `AYNI_BUDGET_EXHAUSTED` | 预算余额不足 | 等待周期重置或调整预算 |
| -14 | `AYNI_STORAGE_FAILED` | 持久化失败；相关输出未释放 | 查看诊断；Kernel 可能已进入受限栅栏 |
| -15 | `AYNI_REVISION_CONFLICT` | `expected_revision` 或依赖版本已过时 | 读取新版本，重新评估后再提交 |
| -16 | `AYNI_IDEMPOTENCY_CONFLICT` | 同一幂等键对应了不同的规范化请求身份（旧普通命令兼容摘要） | 换新的幂等键表示新意图 |
| -17 | `AYNI_RESULT_UNKNOWN` | 已越过可能派发边界，结果无法确定 | 查询或人工处理；不得自动重试（C-STATUS-004） |
| -18 | `AYNI_INTERNAL` | Core 检测到自身不变量被破坏 | 报告缺陷；Kernel 可能进入 `RESTRICTED` |

## 原因码

`ayni_error_v1.reason` 在状态码之外说明拒绝的原因，`object` 指出它涉及的对象（ID、版本与种类）。每个原因码只属于一个状态码；调用方不认识的原因码按其状态码处理。目前由 `ayni_submit_intent` 输出（`include/ayni/ayni.h`）；自 0.8 起 `ayni_goal_info_v1` 的 `skip_reason`、`wait_reason` 也用其中的 `ID_EXHAUSTED` 与只用于 Goal 信息的 `TASK_CAPACITY` 说明 occurrence 为什么跳过或等待（ADR-0020）；自 0.10 起 `ayni_task_info_v1` 的 `hold_status`、`hold_reason` 说明策略的决定为什么使 Task 停下，原因码为只用于 Task 信息的 28–30 与 `INTENT_UNSUPPORTED`、`TASK_REVISION`、`REVISION_EXHAUSTED`，策略自己返回的其他失败配 `NONE`，不是状态码的返回值（大于 `AYNI_OK`）记为 `INVALID_ARGUMENT` 配 `NONE`，所以 `hold_status` 总是失败；Goal 信息的 `skip_reason`、`wait_reason` 另用只用于 Goal 信息的 `TASK_HELD`（31）说明停下的 Task 使 Goal 不派生（ADR-0022）；自 0.12 起 Task 信息另用只用于 Task 信息的 `STEP_TAKEN`（32）与 `STEP_CAPACITY`（33）说明报告进度的意图为什么被拒绝（ADR-0024）。自 0.14 起，恢复时本次启动不能服务的对象也用原因码说明（ADR-0026）：`ayni_agent_info_v1.fault_reason` 用只用于实例信息的 `AGENT_UNREGISTERED`（34），`ayni_goal_info_v1.hold_reason` 用 `GOAL_TYPE_UNKNOWN` 与 `GOAL_TYPE_VERSION`，Task 信息另用只用于 Task 信息的 `STRATEGY_CHANGED`（35）；`ayni_submit_intent` 另输出命令幂等的 `COMMAND_EXPIRED`（36）、`COMMAND_CONFLICT`（37）与存储缓冲没有空间时的 `STORAGE_BACKLOG`（38），不再输出 `COMMAND_ID_UNSUPPORTED`。

| 值 | 名称 | 状态码 | 含义 |
|---:|---|---|---|
| 0 | `AYNI_REASON_NONE` | 任意 | 状态码之外没有更多说明 |
| 1 | `AYNI_REASON_SESSION_UNKNOWN` | `DENIED` | `session_id` 不是已打开的会话 |
| 2 | `AYNI_REASON_COMMAND_ID_UNSUPPORTED` | `UNSUPPORTED` | 0.14 之前：非零 `command_id`；自 0.14 起不再返回（ADR-0026） |
| 3 | `AYNI_REASON_INTENT_UNSUPPORTED` | `UNSUPPORTED` | 本库未实现的意图种类 |
| 4 | `AYNI_REASON_GOAL_TYPE_UNKNOWN` | `NOT_FOUND` | 没有这个 GoalType ID |
| 5 | `AYNI_REASON_GOAL_TYPE_VERSION` | `UNSUPPORTED_VERSION` | GoalType ID 已注册，版本未注册 |
| 6 | `AYNI_REASON_GOAL_PARAMETERS` | `INVALID_ARGUMENT` | 参数过大，或被 GoalType 拒绝 |
| 7 | `AYNI_REASON_OWNER_UNKNOWN` | `NOT_FOUND` | `owner_instance_id` 不是本 Kernel 的实例 |
| 8 | `AYNI_REASON_PARENT_UNKNOWN` | `NOT_FOUND` | `parent_goal_id` 不存在 |
| 9 | `AYNI_REASON_PARENT_STATE` | `INVALID_STATE` | 父目标既不是 `ACTIVE` 也不是 `PAUSED` |
| 10 | `AYNI_REASON_PARENT_OWNER` | `INVALID_ARGUMENT` | 子目标的 owner 与父目标不同 |
| 11 | `AYNI_REASON_GOAL_DEPTH` | `CAPACITY_EXCEEDED` | 子目标会超过 `goal_depth_max` |
| 12 | `AYNI_REASON_GOAL_CHILDREN` | `CAPACITY_EXCEEDED` | 父目标已有 `goal_children_max` 个未终结子目标 |
| 13 | `AYNI_REASON_GOAL_CAPACITY` | `CAPACITY_EXCEEDED` | 已有 `goal_capacity` 个 Goal；自 0.13 起其中没有可让位的终态 Goal（ADR-0025） |
| 14 | `AYNI_REASON_ID_EXHAUSTED` | `CAPACITY_EXCEEDED` | 本次启动的 ID 已分配完（C-ID-010） |
| 15 | `AYNI_REASON_GOAL_UNKNOWN` | `NOT_FOUND` | `goal_id` 不存在 |
| 16 | `AYNI_REASON_GOAL_REVISION` | `REVISION_CONFLICT` | `expected_revision` 不是目标的当前版本 |
| 17 | `AYNI_REASON_GOAL_STATE` | `INVALID_STATE` | 目标的生命周期不允许该变化 |
| 18 | `AYNI_REASON_REVISION_EXHAUSTED` | `CAPACITY_EXCEEDED` | 该变化要提升的某个 Goal、Task 或实例的版本已是最大值（C-COMMIT-012） |
| 19 | `AYNI_REASON_TRIGGER_KIND` | `INVALID_ARGUMENT` | GoalType 不接受该触发种类（ADR-0018） |
| 20 | `AYNI_REASON_OWNER_STATE` | `INVALID_STATE` | owner 实例已 `STOPPED` 或 `FAULTED` |
| 21 | `AYNI_REASON_TASK_UNKNOWN` | `NOT_FOUND` | `task_id` 不是内存中的 Task（含已让位的 Task） |
| 22 | `AYNI_REASON_TASK_REVISION` | `REVISION_CONFLICT` | `expected_revision` 不是 Task 的当前版本 |
| 23 | `AYNI_REASON_TASK_STATE` | `INVALID_STATE` | Task 的生命周期不允许该变化 |
| 24 | `AYNI_REASON_INSTANCE_UNKNOWN` | `NOT_FOUND` | `instance_id` 不是本 Kernel 的实例 |
| 25 | `AYNI_REASON_INSTANCE_REVISION` | `REVISION_CONFLICT` | `expected_revision` 不是实例的当前版本 |
| 26 | `AYNI_REASON_INSTANCE_STATE` | `INVALID_STATE` | 实例的生命周期不允许该变化 |
| 27 | `AYNI_REASON_TASK_CAPACITY` | `CAPACITY_EXCEEDED` | 只用于 Goal 信息：Task 表没有槽位，occurrence 不能成为 Task（ADR-0020） |
| 28 | `AYNI_REASON_POLICY_OUTPUT_LIMIT` | `CAPACITY_EXCEEDED` | 只用于 Task 信息：决定的意图超过策略的 `max_intents_per_call`，私有检查点超过它的 `max_checkpoint_bytes`（自 0.12），或策略返回 `CAPACITY_EXCEEDED`（ADR-0022、ADR-0024） |
| 29 | `AYNI_REASON_POLICY_INTENT` | `INVALID_ARGUMENT` | 只用于 Task 信息：某个意图的 `struct_size` 不符，或两个意图都要结束 Task；自 0.12 起还有 `REPORT_PROGRESS` 的 `step_key` 不是目录 ID，或结束意图带 `step_key`（ADR-0022、ADR-0024） |
| 30 | `AYNI_REASON_POLICY_NO_PROGRESS` | `INVALID_ARGUMENT` | 只用于 Task 信息：策略返回 `AYNI_OK` 却没有意图，Task 没有可等的东西（ADR-0022） |
| 31 | `AYNI_REASON_TASK_HELD` | `INVALID_STATE` | 只用于 Goal 信息：策略的决定使 Goal 的一个 Task 停下，它结束前 Goal 不派生新 Task（ADR-0022） |
| 32 | `AYNI_REASON_STEP_TAKEN` | `INVALID_ARGUMENT` | 只用于 Task 信息：`REPORT_PROGRESS` 指名的步骤已由 Task 或同一决定中前面的意图提交（C-POLICY-008，ADR-0024） |
| 33 | `AYNI_REASON_STEP_CAPACITY` | `CAPACITY_EXCEEDED` | 只用于 Task 信息：决定会使 Task 的步骤超过 `task_step_capacity`（ADR-0024） |
| 34 | `AYNI_REASON_AGENT_UNREGISTERED` | `NOT_FOUND` | 只用于实例信息：日志中有本次启动没有注册的 Agent 的实例，它因此为 `FAULTED`（ADR-0026） |
| 35 | `AYNI_REASON_STRATEGY_CHANGED` | `UNSUPPORTED_VERSION` | 只用于 Task 信息：恢复的 Task 的 GoalType 本次没有策略，或它的私有检查点不是该策略当前版本写的（C-POLICY-007，ADR-0026） |
| 36 | `AYNI_REASON_COMMAND_EXPIRED` | `EXPIRED` | `command_id` 未映射且不大于该 principal 的下限，命令可能已被接受过（C-WIRE-005，ADR-0026） |
| 37 | `AYNI_REASON_COMMAND_CONFLICT` | `IDEMPOTENCY_CONFLICT` | 该 principal 以同一 `command_id` 接受过内容不同的命令（C-WIRE-004，ADR-0026） |
| 38 | `AYNI_REASON_STORAGE_BACKLOG` | `BUSY` | 缓冲不足、异常重试或快照维护暂停新 transition；已映射命令仍先返回首次结果（ADR-0027） |
| 39 | `AYNI_REASON_STORAGE_SPACE` | `STORAGE_FAILED` | 可用介质空间不足以接纳新的 transition 及维护预留，拒绝发生在状态改变前（ADR-0027） |
| 40 | `AYNI_REASON_LEGACY_UNCONFIRMED` | `DENIED` | 恢复的活工作缺少已确认授权元数据，保留进度并禁行；明确审批随 MA-006 E/F（ADR-0028） |
| 41 | `AYNI_REASON_CAPABILITY_UNKNOWN` | `NOT_FOUND` | 没有这个 Capability ID |
| 42 | `AYNI_REASON_CAPABILITY_VERSION` | `UNSUPPORTED_VERSION` | Capability ID 已知，版本未注册 |
| 43 | `AYNI_REASON_GRANT_UNKNOWN` | `NOT_FOUND` | 没有保留这个 Grant ID |
| 44 | `AYNI_REASON_GRANT_REVISION` | `REVISION_CONFLICT` | parent 或首次新撤销目标的当前 revision 与请求不符 |
| 45 | `AYNI_REASON_GRANT_INPUT` | `INVALID_ARGUMENT` | Grant scope、期限或root revision等输入不合法 |
| 46 | `AYNI_REASON_GRANT_SCOPE` | `DENIED` | 委派范围超出parent |
| 47 | `AYNI_REASON_GRANT_PARENT` | `DENIED` | 完整父链不能委派该权限 |
| 48 | `AYNI_REASON_GRANT_DEPTH` | `CAPACITY_EXCEEDED` | 超过配置的委派边数 |
| 49 | `AYNI_REASON_GRANT_CAPACITY` | `CAPACITY_EXCEEDED` | 无空闲或安全可回收Grant槽 |
| 50 | `AYNI_REASON_GRANT_NOT_DURABLE` | `BUSY` | 授权事实尚未持久，不能使用或委派 |
| 51 | `AYNI_REASON_GRANT_EXPIRED` | `DENIED` | Grant已到期或为旧boot限时授权 |
| 52 | `AYNI_REASON_GRANT_CATALOG_CHANGED` | `UNSUPPORTED_VERSION` | exact Capability完整注册域改变，旧Grant暂时禁用 |
| 53 | `AYNI_REASON_GRANT_SUBJECT` | `DENIED` | 接收实例不可用 |
| 54 | `AYNI_REASON_GRANT_CHANNEL` | `DENIED` | 需要可信管理端Session |
| 55 | `AYNI_REASON_GRANT_UNSUPPORTED` | `UNSUPPORTED` | Scene或额度授权尚未实现 |
| 56 | `AYNI_REASON_GRANT_REVOKED` | `DENIED` | 目标或任一祖先有 Gate/已撤销 |
| 57 | `AYNI_REASON_REVOCATION_PENDING` | `BUSY` | 同 target 已有另一条待决撤销，不占新命令身份 |
| 58 | `AYNI_REASON_COMMAND_PINNED` | `BUSY` | 该 principal 的结果窗口全部被已接受撤销 pin 住 |
| 59 | `AYNI_REASON_REVOCATION_CAPACITY` | `CAPACITY_EXCEEDED` | 有界 Gate/pending 容量不足，原子拒绝 |

| 60 | `AYNI_REASON_GOAL_REQUIREMENT_INPUT` | `INVALID_ARGUMENT` | 需求描述或 Scope 非法；Since 0.21 |
| 61 | `AYNI_REASON_GOAL_REQUIREMENT_CAPACITY` | `CAPACITY_EXCEEDED` | 需求数或字节区超出配置；Since 0.21 |
| 62 | `AYNI_REASON_GOAL_AUTHORITY_MISSING` | `DENIED` | 没有完整已选授权证据；Since 0.21 |
| 63 | `AYNI_REASON_GOAL_AUTHORITY_PAUSED` | `DENIED` | 需显式重新授权，普通恢复不解除；Since 0.21 |
| 64 | `AYNI_REASON_GOAL_AUTHORITY_PARENT` | `DENIED` | 子 Goal 超出原父需求上限；Since 0.21 |
| 65 | `AYNI_REASON_GRANT_GOAL` | `DENIED` | Grant 绑定了其他、已结束或不存在的 Goal；Since 0.22 |
| 66 | `AYNI_REASON_GOAL_APPROVAL_CHANGED` | `DENIED` | 批准的完整创建回显或原缺项集合与待批 Goal 不同；Since 0.23 |
| 67 | `AYNI_REASON_GOAL_REAUTHORIZATION_CHANGED` | `DENIED` | 重新授权的完整原创建回显与保留 Goal 不同；Since 0.26 |
| 68 | `AYNI_REASON_GOAL_AUTHORITY_REVISION` | `REVISION_CONFLICT` | 预期的授权事实提交序号已改变；Since 0.26 |
| 69 | `AYNI_REASON_GOAL_AUTHORITY_NOT_DURABLE` | `BUSY` | 新绑定尚未持久，暂不开放派生或策略消费；Since 0.26 |

以上41–55自ABI0.18发布，56–59自ABI0.19发布；C/D实施与验证状态分别见ADR-0030/0031。表格记录状态归属，不表示全部调用路径已经验证。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STATUS-001 | 每个公开函数在文档中列出它可能返回的全部状态码；返回列表外的值视为缺陷 | `ayni_create` 在内存不足时返回 `AYNI_INTERNAL` | M0 已实现：`include/ayni/ayni.h` 为每个已实现函数列出返回码，`tests/unit/kernel_api_test.cpp` 覆盖 `ayni_create` 文档列出的每个失败码；`MA-005` 生命周期返回值表：`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.step_rejects_bad_arguments_without_effect`、`KernelLifecycle.kernel_info_checks_its_output`；工作模型：`WorkModel.return_values_follow_the_state_table`、`WorkModel.intent_arguments_are_refused_before_anything_else`、`WorkModel.registration_checks_its_arguments`、`WorkModel.queries_check_their_arguments`、`WorkTask.task_and_instance_bodies_are_checked_before_anything_else`、`WorkTask.task_query_checks_its_arguments`、`WorkTask.task_and_instance_intents_outside_running_are_not_ready` |
| C-STATUS-002 | 失败调用不产生部分效果：输出参数要么不写，要么写入文档规定的值（如 `*out_kernel = NULL`），调用方内存保持不变 | 版本不符时 `ayni_create` 已经清零了调用方的内存块 | M0 已实现：`KernelApi.failed_create_leaves_memory_untouched` |
| C-STATUS-003 | 状态码区分“参数错”（`INVALID_ARGUMENT`）、“时机不对”（`INVALID_STATE`/`NOT_READY`/`BUSY`）、“不支持”（`UNSUPPORTED*`）与“被拒绝”（`DENIED`/`EXPIRED`/`STALE`/`BUDGET_EXHAUSTED`），不合并为通用失败 | 所有失败都返回 `-1`，调用方无法决定是否重试 | 评审；`MA-005` |
| C-STATUS-004 | `AYNI_RESULT_UNKNOWN` 不是失败：调用方不得因它自动重试或换身份重做，只能查询、对账或交给人工 | Provider 超时后 Kernel 自动用新 `operation_id` 重发 | E2 F3–F5 |
| C-STATUS-005 | `AYNI_NOT_FOUND` 只说明本机查不到记录；只有满足 C-CALL-010 的权威未执行结论才能推断未执行 | 远端查询返回 `NOT_FOUND`，调用方据此重新派发运动 | E2 查询夹具（“仅未找到记录”响应） |
| C-STATUS-006 | 可选的 `ayni_error_v1` 输出原因码与对象引用，保留分类原因；不包含凭据、私有记忆正文、媒体或个人数据 | 错误详情里带出 API Key 或用户语音转写文本 | M1 已实现（`ayni_submit_intent`：每次返回都写状态、原因码与对象引用，只含 ID 与版本）：`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written`、`WorkModel.intent_through_a_closed_or_unknown_session_is_denied`、`WorkModel.unknown_goal_type_or_version_is_refused`、`WorkModel.child_goals_follow_their_parent_and_the_tree_limits`；日志与诊断输出的测试随其实现（M1）；`MA-005` |
| C-STATUS-007 | 状态码与原因码是 ABI 的一部分：新增状态码只能取尚未使用的负数，新增原因码只能取尚未使用的正数，已发布值不重新编号 | 新版本把 `-6` 从 `BUSY` 改为 `DENIED` | 评审；C-VER-002 |

ABI 0.25 起，`ayni_post_event` 在同一 ingress 实例的 u64 输入顺序耗尽时返回 `AYNI_CAPACITY_EXCEEDED`，不入队、不 wake。该上限由新进程的新 ingress 实例重建，自动存储重载不重置；队列暂满继续返回 `AYNI_BUSY`。这不增加状态码或 reason，见 `limits.md` 与 ADR-0038。
