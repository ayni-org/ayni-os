# AP-06/AP-04 缺口清单、实施设计与双实例验收

- **基线**：AP-05 #116；实验性 ABI 0.51、Core schema 7。
- **本包**：用户 2026-10-30 授权盘点、设计、双实例验收及基础验收；选择 Binding 资源映射、开发期直接升级，无旧格式兼容层。见 [ADR-0087](../adr/0087-binding-resource-mapping-and-foundation-acceptance.md)。
- **状态**：实现、当前完整软件回归、最小正常重开及独立审查已完成，基础报告已具备交付证据；最终提交 CI 与交付状态见 #117。最终验收及未验证范围集中在[基础报告](../../reports/core/foundation-acceptance.md)，本设计不把源码盘点当执行结果。

## 1 实际消费者与缺口

| 调用链/属主 | 基线实现与证据 | 缺口及本包处理 |
|---|---|---|
| `include/ayni/ayni.hpp`、`values.hpp`、`calls.h` | 无异常/RTTI 生命周期、有界值和 C99 权威接口；SDK smoke 与五个示例 | 复用既有 facade，不另建注册框架；新增映射由 C API 明确表达，消费者全部重编译 |
| `catalog/binding` → `kernel/config/catalogs` | 冻结配置、精确 Capability/Provider/endpoint/version 匹配 | Capability 资源 ID 全实例共用，无法独立声明实际资源；Binding 拥有有界 ID 映射，解析一个有效契约 |
| `kernel/service/call` → `work/call` | 接受、授权、持久屏障、资源队列、原调用身份与结果 | 接受/准入/派发统一使用解析后的契约，Call 保存实际组；原业务证明保留逻辑需求，重放本征校验允许仅有单位保持的资源置换 |
| Provider、模型 codec、ToolLoop | Provider offer 与逻辑契约精确匹配；模型只产生固定工具和业务值 | 无新增业务分支或模型决定资源映射的端口；已有目录/工具继续走相同 Binding 解析 |
| `providers/software` → `runtime/examples` | 有界分块求和及 I64 事件流，step 外推进 | 保留实现及其生命周期；具体 Host/Provider/应用仍只由 runtime 装配 |
| `examples/managed_operations.cpp` | 原样例只用 Sim，成功检查未核对实际求和值，观察标记位于应用实例 | 工作流归 `applications/software_workflow`，观察证据归 Task checkpoint；验证实际求和值及样本；同一应用分别经 Native/Sim 运行 |
| Native/Sim、助理、实验与测试消费者 | 已有公共 API、正常与历史专项回归 | 迁移 Binding 布局，保留原算法；Native 示例补真实异步文件存储与正常排空，不启动 Linux 系统集成实验 |
| schema/记录 reader、writer、SDK 文档 | schema 7 的 Call 已保存完整资源组和原业务证明 | schema 8 区分新的资源解释；记录字节布局复用，拒旧保留目录，无迁移器；同步契约与版本 |
| 适用回归、覆盖与验收 | M1 和 AP-02/03/05 分别有冻结版本证据 | 当前版本重新汇总；覆盖偏差和新格式恢复专项分别列明，历史 ≥80% 接受不自动续用 |

## 2 Binding 映射与单一有效执行契约

Capability/Provider offer 继续声明同一逻辑资源组及单位。`ayni_binding_v1.resources` 只替换有界、非空逻辑 ID，未映射的需求保持原 ID。映射立即复制，不借用部署数组；仅 CONFIGURING 注册，启动后不可修改。

键不可重复，必须存在于原组；映射同时生效，不递归解析。不增加/删除需求，不改单位，不合并同目标需求。解析后交给现有资源规范化机制排序和查重，再解析实际资源目录，检查容量、冲突域及执行能力。缺失资源、未知逻辑需求、重复目标、容量不足均明确拒绝，不产生 Provider I/O。

`catalog::BindingRegistry::resolve` 返回索引和有效契约。Provider 精确匹配仍以逻辑契约为准；权限域、业务 Schema、effects、form、cancel、query、大小及超时都不由映射改变。Call 接受与资源准入使用同一个有效值，原子组顺序与实际资源索引保持一致；目录与模型视图复用相同解析，不维护旁路目录。

## 3 持久事实及兼容

Call 原身份继续保存确切 Binding/Provider/端点/版本；既有契约字段保存规范化实际资源 ID 与单位，原业务证明保存逻辑组。因此没有第二份持久映射表或资源占用账本。

本征验证要求原证明和有效契约的非资源语义完全一致、资源数量一致，单位存在一一对应；两个组分别规范化。由于原子组本身无顺序语义，单位匹配无需保存配置数组顺序。派发仍要求当前确切身份及解析后的完整有效契约与历史 Call 完全相等；新目录不得改变已接受调用的实际组。相同有效契约的显式恒等映射不引入不同执行语义。

ABI 0.52、Core schema 8 分别升级。Binding 配置布局变化；Call 记录字节布局不变而资源含义改变，故仍升级 schema。策略格式 4、SQLite user_version 2 没有新变化。本包全仓重编译；旧目录拒绝并保留，不加入旧 reader、迁移或自动清空。回退须使用原二进制及原目录。

所有者对原 operation 的只读查询不重新获取执行资源，也不要求新目录能执行旧调用。它按原 Provider/端点/版本和原逻辑业务证明匹配，接受 Call 中已经验证的单位保持实际组；普通 Binding 注册仍要求逻辑契约精确匹配。静态边界用例验证此区别，既有查询回归保持，不新增 UNKNOWN 注入活动。

## 4 AP-06 应用、SDK 与生命周期边界

应用仅注册策略/GoalType、提出业务请求及解释结果，不选 Host、Provider 或存储路径。软件 Provider 通过公开 usage 数据描述自身语义，可信 runtime 经原注册端口采纳，应用首次工作从 Task 目录 inspect 精确版本和原业务证明。工作流用 Task 私有一字节 checkpoint 记录已观察样本，避免多个 Task 共用实例标记。求和值按公开 `ayni_value_read` 解码并精确检查；UNKNOWN/失败不得作为业务成功。

runtime 选择 Sim 或 Native、软件 Provider 与样例应用；客户端仍仅包含公开 API/runtime 头。Native 示例目录由测试创建并在显式 close 后清理，使用真实单调时间和异步文件存储；Sim 使用显式时间与完成推进。二者共用应用、规范输入、Grant、结果与关闭规则。

所有配置、推进、策略回调与关闭在 owner 线程；Provider 复制接受的请求，正常终态投递后释放完成借用。排空发生在 Kernel step 外。Native 文件生产者在 CONFIGURING/STOPPED 的 quiesce 中 join，Host/Provider/应用上下文保持到成功 destroy。初始化或关闭失败保留现有分类错误及借用，不能伪造成功。

## 5 双实例与基础验收场景

| 场景 | 可观察判据 | 验证入口 |
|---|---|---|
| 同语义两个端点、独立资源 | 两 Agent 的同能力调用均被各自端点接受；结果精确对应，Call ID 不同 | `BindingInstances.exact_routes_with_independent_resources_run_concurrently` |
| 同语义两个端点、共享资源 | 第二调用保持 QUEUED，释放后交原端点；沿用已接受 Call ID | `BindingInstances.shared_resource_queues_second_instance_until_release` |
| 映射复制与边界 | 栈上 ID 修改不影响结果；非法 count/pointer/reserved、重复键、未知需求及合并拒绝 | `BindingInstances`、`ProviderApi.binding_*` |
| 原证明与实际组 | 保持单位多重性；改单位/数量/超时等不构成合法资源置换 | `CallContract.binding_resource_substitution_keeps_semantics_and_unit_multiplicity` |
| 同应用跨 Host | 精确分块求和、有效事件样本、有限等待与关闭，Native 实际写出日志 | `Examples.managed_operations`、`Examples.managed_operations_native` |
| 最小正常重开 | 旧调用/真实结果保持；新代订阅保留原实际组，改变映射后原 Task 暂停且无新开启 | `BindingInstances.orderly_reopen_retains_mapped_call_and_exact_result`、`MappedSubscription.orderly_reopen_keeps_original_actual_resource_group`、`MappedSubscription.changed_actual_group_holds_original_call_without_new_opening` |
| 既有通用能力 | typed/授权/目录/模型工具/管理操作及历史 E1～E4 回归保持 | 最终完整 CTest、适用 CI 和审查；命令、数量及运行链接集中 PR |

用户2026-10-31明确允许仅本包最小正常重开/目录映射不兼容验证，已执行；其他新增故障/恢复、断电及变异专项仍暂停，历史回归保留。该证据不扩为 schema 8 全部故障/恢复通过。基础报告逐条核对当前计划六项门槛，缺失的必要证据或覆盖偏差须明确决定，不能为交付目标自行降低标准。
