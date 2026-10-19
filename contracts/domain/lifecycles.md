# 生命周期与状态转换

- **关联**：计划 §2.4、§3.2「生命周期」、§3.6、§4.3、§4.4；ADR-0006、ADR-0007；`MA-004`、`MA-005`；E2 F13，E3 步骤 5
- **原则**：生命周期、持久化状态与等待原因是三个维度。生命周期写在枚举里；“是否已持久接受”由提交序号表达（C-COMMIT-004）；等待原因由待决调用派生（ADR-0007）。表中未列出的转换都是非法转换，返回 `AYNI_INVALID_STATE` 并保持原状态。

## Kernel

| 状态 | 值 | 含义 |
|---|---:|---|
| `CONFIGURING` | 1 | 已创建，可以注册目录；不处理意图 |
| `RECOVERING` | 2 | `ayni_start` 已冻结目录并通过交叉引用校验，`step` 正在异步重放持久前缀 |
| `RUNNING` | 3 | 开放准入 |
| `RESTRICTED` | 4 | 恢复发现不兼容、损坏或存储不可写；只读诊断，本地停止路径照常可用 |
| `DRAINING` | 5 | `ayni_begin_shutdown` 之后，收敛在途工作，不再接受新意图 |
| `STOPPED` | 6 | 停机完成，可以 `ayni_destroy` |

| 从 | 到 | 触发 |
|---|---|---|
| — | `CONFIGURING` | `ayni_create` 成功 |
| `CONFIGURING` | `RECOVERING` | `ayni_start` 校验通过 |
| `CONFIGURING` | `CONFIGURING` | `ayni_start` 校验失败（返回错误与 `ayni_error_v1`，可修正后重试） |
| `RECOVERING` | `RUNNING` | 持久前缀重放完成、幂等映射重建、授权与资源检查完成 |
| `RECOVERING` | `RESTRICTED` | 版本不支持、已提交区域损坏、关键记录无法验证 |
| `RUNNING` | `RESTRICTED` | 受限栅栏中无法确认旧写入方已排空或隔离，或存储仍不可写（C-COMMIT-008） |
| `RUNNING` | `RECOVERING` | 受限栅栏确认旧写入方已隔离，以新 `boot_epoch` 重载 |
| `RUNNING`、`RECOVERING`、`RESTRICTED` | `DRAINING` | `ayni_begin_shutdown` |
| `DRAINING` | `STOPPED` | 在途工作已终结或记录为未知，资源已释放或隔离 |

各状态下公开函数的返回值（M0 已实现的函数在前，其余为计划）：

| 函数 | CONFIGURING | RECOVERING | RUNNING | RESTRICTED | DRAINING | STOPPED |
|---|---|---|---|---|---|---|
| `ayni_get_lifecycle` | OK | OK | OK | OK | OK | OK |
| `ayni_destroy` | OK | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | OK |
| `ayni_register_*` | OK 或参数错误 | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| `ayni_start` | OK 或校验错误 | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| `ayni_submit_intent` | NOT_READY | NOT_READY | 按意图校验 | NOT_READY | NOT_READY | NOT_READY |
| 查询 | INVALID_STATE | OK（标注 `durable_seq`） | OK | OK（只读诊断） | OK | INVALID_STATE |
| `ayni_post_event` | INVALID_STATE | OK | OK | OK（只用于对账与诊断） | OK | INVALID_STATE |
| `ayni_step` | INVALID_STATE | OK | OK | OK | OK | INVALID_STATE |
| `ayni_begin_shutdown` | INVALID_STATE | OK | OK | OK | OK（幂等） | OK（幂等） |

## AgentInstance

`REGISTERED → STARTING → RUNNING ↔ PAUSED → STOPPING → STOPPED`；初始化或检查点不兼容时进入 `FAULTED`，或带原因进入 `PAUSED`。结束一次任务不结束实例。

## Goal

`PROPOSED → ACTIVE ↔ PAUSED → SATISFIED / CANCELLED / EXPIRED / FAILED`。需要确认的 Goal 停留在 `PROPOSED`，不生成可执行工作；持续目标在 `ACTIVE` 期间可以多次产生 Task。

## Task

`READY ↔ RUNNING → COMPLETED`；也可以进入 `PAUSED`、`CANCELLED`、`FAILED`、`EXPIRED`；异步取消尚未完成时为 `CANCELLING`。等待模型、资源、审批或动作是 `blocked_on` 视图，可以同时存在多项，不写入生命周期。

## ActionRecord

| 从 | 到 | 条件 |
|---|---|---|
| — | `PROPOSED` | 意图通过 Schema 校验 |
| `PROPOSED` | `PREPARED` | 授权与参数通过，准备记录（意图、步骤身份、绑定）持久化 |
| `PROPOSED` | `REJECTED` | 授权、参数或预算拒绝 |
| `PREPARED` | `DISPATCHING` | 再次校验通过，可能派发屏障持久化成功（C-COMMIT-006） |
| `PREPARED` | `CANCELLED` | 取消或条件失效（确定未派发） |
| `DISPATCHING` | `IN_PROGRESS` | Host 报告执行中 |
| `DISPATCHING` | `CANCELLED` | 屏障后复核拒绝且可证明未交给 Host，`effect=NONE` 已提交（C-COMMIT-007） |
| `DISPATCHING`、`IN_PROGRESS` | `COMPLETED` | 完成结果持久化 |
| `IN_PROGRESS` | `FAILED` | 确定失败结果持久化 |
| `DISPATCHING`、`IN_PROGRESS` | `RESULT_UNKNOWN` | 重启或无法确认结果 |
| `RESULT_UNKNOWN` | `COMPLETED` / `FAILED` | 权威查询确认（C-CALL-010） |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-LIFE-001 | `ayni_create` 成功后 Kernel 处于 `CONFIGURING` | 创建后直接处于 `RUNNING` 并开始接受意图 | M0 已实现：`KernelApi.create_in_caller_memory_starts_configuring` |
| C-LIFE-002 | `ayni_destroy` 只在 `CONFIGURING` 或 `STOPPED` 成功；其他状态返回 `AYNI_INVALID_STATE` 且 Kernel 不变 | 运行中的 Kernel 被销毁，在途 I/O 回调写入已释放的内存 | M0 已实现 `CONFIGURING` 路径：`KernelApi.kernel_memory_is_reusable_after_destroy`；其他状态待 `MA-005` |
| C-LIFE-003 | 注册只在 `CONFIGURING` 进行；`ayni_start` 一次性校验全部交叉引用，注册顺序无关；引用缺失时 start 失败并保持 `CONFIGURING` | 先注册 Binding 后注册 Capability 导致失败 | `MA-005` |
| C-LIFE-004 | 非 `RUNNING` 时 `ayni_submit_intent` 返回 `AYNI_NOT_READY`，不排队、不改变状态 | 恢复期间到达的创建提醒命令被排队，恢复后又按重建的幂等映射再次创建 | E2 F13 |
| C-LIFE-005 | 生命周期变化经事件与 `ayni_get_lifecycle` 同时可见；事件在状态已应用之后发出 | 事件报告 `RUNNING` 时查询仍返回 `RECOVERING` | `MA-005` |
| C-LIFE-006 | 首版不提供运行时注销；运行时发现的目录变化另行定义为经提交协调的 transition | 运行中删除 Capability，在途 ActionRecord 失去语义 | `MA-005` |
| C-LIFE-010 | ActionRecord 只按上表转换；`RESULT_UNKNOWN` 只能经权威查询离开 | `RESULT_UNKNOWN` 超时后自动改为 `FAILED` 并重试 | E2 F3–F5；状态转换表测试 |
| C-LIFE-011 | 运行终态与副作用结果分开：`CANCELLED`/`FAILED` 的记录列出已完成、未执行与未知的步骤，不推导“没有影响” | 取消后报告“无影响”，但第一步转向已执行 | E4 步骤 3 |
| C-LIFE-012 | 取消请求已受理、停止指令已送达、物理输出已停止是三种事实，分别表达 | 把“已发送停止”显示为“已停止” | E1 步骤 4；C-STOP-006 |
| C-LIFE-013 | 撤销响应区分 `ACCEPTED`、`DURABLY_EFFECTIVE`、`CANCELLING`、`STOP_CONFIRMED`；只有最后一项证明本地物理停止 | 授权版本生效即报告设备已停止 | E1 步骤 6；E2 F10 |
| C-LIFE-020 | 已接受的 Goal/Task 固定 owner 实例与设备；失联不自动转交其他实例 | 设备 A 离线后，设备 B 上的同名实例接管 A 的提醒 | `MA-004`；`MA-016` |
| C-LIFE-021 | 暂停 Goal 保留目标与检查点，停止产生新 Task；在途 Task 按该类型声明的暂停点收敛；无法立即暂停时显示“暂停中” | 暂停目标后仍派生新 Task | `MA-004` 传播矩阵 |
| C-LIFE-022 | 取消 Goal/Task 标记取消版本，禁止新增派生步骤，并传播到所属在途请求；已完成影响保留，补偿是另一次需授权的操作 | 取消后自动反向转动以“回滚”物理世界 | `MA-004`；E4 步骤 3 |
| C-LIFE-023 | 实例暂停阻止其新增工作并到兼容检查点；停止收敛全部所属工作并回收资源；“暂停助理”不映射为暂停全部实例（C-PROD-020） | 暂停助理时把提醒所属实例一并暂停 | E1 步骤 11 |
| C-LIFE-024 | 不兼容的私有检查点或未知策略版本使 Task 暂停并报告原因，保留原记录，不重置 Task 或改写完成条件 | 换策略失败后 Task 被重置为 `READY` 重新开始 | E3 步骤 5 |
