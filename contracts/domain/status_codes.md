# 状态码

- **关联**：计划 §3.2「错误」、§3.4；ADR-0006、ADR-0017、ADR-0018；`MA-004`、`MA-005`
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
| -16 | `AYNI_IDEMPOTENCY_CONFLICT` | 同一幂等键对应了不同的规范化参数摘要 | 换新的幂等键表示新意图 |
| -17 | `AYNI_RESULT_UNKNOWN` | 已越过可能派发边界，结果无法确定 | 查询或人工处理；不得自动重试（C-STATUS-004） |
| -18 | `AYNI_INTERNAL` | Core 检测到自身不变量被破坏 | 报告缺陷；Kernel 可能进入 `RESTRICTED` |

## 原因码

`ayni_error_v1.reason` 在状态码之外说明拒绝的原因，`object` 指出它涉及的对象（ID、版本与种类）。每个原因码只属于一个状态码；调用方不认识的原因码按其状态码处理。目前由 `ayni_submit_intent` 输出（`include/ayni/ayni.h`）。

| 值 | 名称 | 状态码 | 含义 |
|---:|---|---|---|
| 0 | `AYNI_REASON_NONE` | 任意 | 状态码之外没有更多说明 |
| 1 | `AYNI_REASON_SESSION_UNKNOWN` | `DENIED` | `session_id` 不是已打开的会话 |
| 2 | `AYNI_REASON_COMMAND_ID_UNSUPPORTED` | `UNSUPPORTED` | 非零 `command_id`；命令幂等随 `MA-010` |
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
| 13 | `AYNI_REASON_GOAL_CAPACITY` | `CAPACITY_EXCEEDED` | 已有 `goal_capacity` 个 Goal（含终态） |
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
