# 原语的最小字段

- **关联**：计划 §2.2、§2.3、§2.7、§3.6；ADR-0001、ADR-0007、ADR-0008、ADR-0017；`MA-004`、`MA-006`、`MA-008`、`MA-011`、`MA-018`
- **说明**：本文固化每个原语在 M1 实现前必须具备的最小字段（计划 §2.2：“M0 固化最小字段、状态与提交不变量”）。编码（字段宽度、可选字段的表示）在 M1 由 fixtures 收敛，选定 IDL 后由 ADR-0009 生成。表中“持久”指写入日志或快照；“重建”指重启后由 Host 重新探测或重新建立，不从持久记录恢复为可用对象。

所有可修改对象都带 `revision`（每次提交的修改递增），作为 `expected_revision` 的比较对象（C-COMMIT-012）。所有对象都带创建它的 `transition_id`，用于追溯。

## AgentInstance（属主 `work/agent`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `agent_id`、`instance_id`、`device_id` | 逻辑身份、驻留实例、所在设备 | 是 |
| `role`、`strategy_ref`（策略 ID 与版本） | 实例角色与绑定的策略 | 是 |
| `grant_refs` | 实例持有的 Grant 引用（Grant 本身在 `authority`） | 是 |
| `lifecycle` | 见 `lifecycles.md` | 是 |
| `pause_reason` / `fault_reason` | 暂停或故障原因码 | 是 |
| `private_namespace_id` | 私有记忆命名空间（在 `knowledge`） | 是 |
| `boot_epoch` | 实例最近一次启动所在的 Kernel 代际 | 是 |

## Goal 与 Task（属主 `work/goal`、`work/task`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `goal_id`、`goal_type_ref`（类型 ID 与版本） | 身份与业务类型 | 是 |
| `origin` | 创建来源：`USER`、`RULE`、`REFLECTION`、`REMOTE_PRINCIPAL` 及主体引用 | 是 |
| `owner_instance_id`、`owner_device_id` | 固定归属（C-LIFE-020） | 是 |
| `parent_goal_id`、`depth` | 父目标与深度（受 C-LIMIT 限制） | 是 |
| `completion_rule`、`expiry` | 完成或持续条件、失效规则（恢复策略，不存单调值） | 是 |
| `dedupe_key`、`trigger_cursor` | 去重键与周期触发游标 | 是 |
| `budget_ref` | 子预算引用（在 `budget`） | 是 |
| `notify_plan` | 通知计划（主方式与获准备选，C-PROD-012） | 是 |
| `lifecycle`、`revision`、`cancel_revision` | 生命周期、版本、取消版本 | 是 |
| Task：`task_id`、`goal_id`、`occurrence_id` | 一次执行及其来源 occurrence | 是 |
| Task：`checkpoint`（共同检查点，C-CALL-004） | 步骤版本、绑定、输入摘要、结果引用、已提交进度、未知状态 | 是 |
| Task：`strategy_checkpoint_ref` | 策略私有检查点（ID、版本、内容引用） | 是 |
| Task：`blocked_on` | 由待决调用派生的视图，不存储（ADR-0007） | 否 |

## Observation（属主 `knowledge/observation`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `observation_id`、`origin_device_id`、`revision` | 身份 | 授权时 |
| `kind`：`MEASUREMENT` 或 `INTERPRETATION` | 测量值与模型推断分开标记 | 授权时 |
| `source_ref` | 传感器 Component、Provider 或上游观察 | 授权时 |
| `captured_at`、`clock_trust` | 采集时间与时钟可信度 | 授权时 |
| `valid_for`、`quality` | 有效期（恢复策略）与质量 | 授权时 |
| `subscription_id`、`generation`、`sequence` | 来自订阅时的代际与序号 | 否 |
| `media_ref` | 受控媒体引用（短期有效，C-PRIM-005） | 否 |
| `related_refs` | 关联观察 | 授权时 |

## Device、Component、Resource、Binding（属主 `catalog`；运行时状态在 `resources`）

| 对象 | 最小字段 | 持久 |
|---|---|---|
| Device | `device_id`、`host_profile`、`firmware_version`、可信注册来源 | 是 |
| Component | `component_id`、`device_id`、`kind`、`calibration_ref` | 是 |
| Resource | `resource_id`、`capacity`、`sharing`（`SHARED`/`EXCLUSIVE`）、`conflict_domain`、是否为控制域、`authority_epoch`（控制域） | 描述持久；在线、占用、健康重建 |
| Capability | `capability_id`、`version`、输入/结果 Schema 引用、调用形态（C-CALL-001）、副作用属性（C-CALL-005）、所需资源、时限、恢复属性 | 是 |
| Binding | `binding_id`、`capability_ref`、`provider_id` 或 Component、所需 Resource、外发目的地、费用类别 | 是 |

## Capability Grant 与主体（属主 `authority`）

| 对象 | 最小字段 | 持久 |
|---|---|---|
| UserPrincipal | `principal_id`、`binding_revision`、认证渠道 | 是 |
| Session | `session_id`、`principal_id`、输入渠道（`VOICE`、`MANAGEMENT`，由 Host 认证后打开会话时给出）、信任级别（由渠道决定，不单独保存） | 否（重启后全部关闭，由 Host 重新打开） |
| Grant | `grant_id`、`subject`、`target`、`scope`（能力、参数约束、Scene）、`expiry`、`quota`、`revoke_revision`、`delegation`（父 Grant 与可否再委派） | 是 |
| Scene | `scene_id`、`version`、`enabled`、授权主体与范围、参数约束、前置条件、观察新鲜度、次数/费用预算、停止条件 | 是 |

## ActionRecord 与 LogicalCall（属主 `work/action`、`work/call`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `logical_call_id`、`task_id`、`step_key` | 语义调用身份 | 是 |
| `operation_id`、`binding_id`、`capability_ref` | 有副作用时的幂等身份与具体绑定 | 是 |
| `grant_ref`、`scene_ref` | 准备时所依据的授权与场景版本 | 是 |
| `argument_digest` | 规范化参数摘要（C-VER-032） | 是 |
| `state` | 见 `lifecycles.md` 的 ActionRecord 状态 | 是 |
| `effect` | 终态的影响：`NONE`、`PARTIAL`、`COMPLETE`、`UNKNOWN`，并列出已完成、未执行、未知的步骤 | 是 |
| `attempts` | `(boot_epoch, request_id)` 列表 | 是（有副作用时） |
| `evidence_refs` | 结果、停止确认与物理验证证据 | 是 |

## Budget（属主 `budget`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `budget_id`、`parent_budget_id` | 层级（设备 → 实例 → 目标 → 任务） | 是 |
| `limits` | 每类计量（模型调用、token、字节、任务数、媒体分析、通知频率）的周期上限 | 是 |
| `spent`、`reserved` | 本周期已结算与待结算预留 | 是 |
| `period` | 周期规则 | 是 |
| `system_reserve` | 停止、诊断与前台交互的保留额度（不可被普通工作占用） | 是 |

## MemoryRecord（属主 `knowledge/memory`）

| 字段 | 含义 | 持久 |
|---|---|---|
| `memory_id`、`namespace_id`、`revision` | 私有命名空间或助理公共档案 | 是 |
| `source_refs`、`confirmation` | 来源与确认状态（已确认/推断） | 是 |
| `visibility` | 共享主体、范围与期限 | 是 |
| `tombstone` | 删除标记与删除版本 | 是 |
| `lineage_refs` | 派生摘要所依赖的源记录与版本 | 是 |

## Subscription、ControlSession、ExecutionPermit

见 `calls_and_bindings.md`（订阅）与 `permits_and_stop.md`（控制会话与许可）。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-PRIM-001 | 物理句柄、指针、资源占用、单调时间值不跨重启恢复为可用对象；重启后由 Host 重新探测或重新准入 | 重启后沿用旧的电机驱动句柄与占用状态 | E2 F1–F4；`MA-018` |
| C-PRIM-002 | 每个原语只有一个属主模块写它的状态；其他模块只通过属主的接口读取或由 `kernel` 在同一 transition 中编排修改 | `budget` 模块直接改 Task 的 `blocked_on` | `check_deps`；评审（ADR-0008） |
| C-PRIM-003 | 可由其他持久字段推导的值不持久化（如 Task 的 `blocked_on`、通知的用户可见状态） | 同时持久化待决调用集合与等待原因枚举，二者不一致 | 评审（ADR-0007 验证项） |
| C-PRIM-004 | 测量值与模型推断分开标记；推断不能替代必须由本机测量证明的物理前提（停止、校准、设备健康） | 远端模型“看到电机已停”被当作停止确认 | E1 步骤 4；C-STOP-006 |
| C-PRIM-005 | 媒体引用绑定主体、用途、有效期、大小与内容类型；引用过期、取消或重启时回收，不在重启后假装仍可访问 | 重启后仍按旧引用读取已回收的图片缓冲 | `MA-015`；E1 步骤 8 |
| C-PRIM-006 | 过期观察不能支持新动作；准入时检查观察的有效期与质量 | 用 10 分钟前的场景变化触发“转向观察” | E2 F1、F10；C-AUTH-011 |
| C-PRIM-007 | 同一逻辑身份在多个设备上的实例各自独立：不共享权限、可变状态，也不自动转移任务或合并记忆 | 设备 B 上的同名实例读取设备 A 实例的私有记忆 | E1 步骤 1；`MA-016` |
| C-PRIM-008 | Component 与 Resource 不一一对应，经 Binding 关联；带 `authority_epoch` 的执行器 Resource 是控制域 | 把“左轮”“右轮”各自作为独立控制域，两个会话同时驱动底盘 | `MA-018`；ADR-0001 |
