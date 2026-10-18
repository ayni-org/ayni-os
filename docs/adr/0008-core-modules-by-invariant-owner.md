# ADR-0008：Core 模块按不变量属主划分

- **状态**：已接受（设计基线，待实现与验证）
- **日期**：2026-10-18
- **关联**：计划 §2.0、§2.3、§2.8、§4.3、§8；`MA-017`

## 背景

v1.3 的计划 §8 按实体名词把 Core 切分为 19 个目录。派发屏障这一关键安全链路分布在 `capability/`、`policy/`、`request/`、`binding/` 与 `state/`；LogicalCall 没有归属；架构图中的 Core 分组也与目录划分不一致。v1.4 首先在原有目录内为每项不变量指定唯一属主，并把本 ADR 记为候选，约定在首个 Core 实现提交前按下述标准决定。

**决定标准**：以 §2.8 的每个提交批次和 §4.3 的动作提交顺序为样例，列出每项不变量的写入方；若在原有目录下仍有不变量需要 3 个或以上目录共同写入，则采纳候选划分。

## 评估

| 不变量 | 来源 | 原有目录下的写入方 | 候选划分下的写入方 |
|---|---|---|---|
| 命令重试返回原对象 | §2.8 批次 1 | 命令幂等映射（未指定属主）、`goal/`、`task/` | `kernel`（幂等映射）、`work` |
| 触发不漏不重 | §2.8 批次 2 | `goal/`、`task/`、`budget/` | `work`；预算预留由 `resources` 独立保证 |
| 步骤调用身份稳定 | §2.8 批次 3 | `task/`、`binding/`、`capability/`/`policy/`；LogicalCall 未指定属主 | `work` |
| 结果只应用一次 | §2.8 批次 4 | `request/`、`task/`、`capability/`/`policy/` | `work` |
| 删除与撤销不复活 | §2.8 批次 5 | `memory/`、`context/`、`observation/`、`request/` | `knowledge`；在途结果拒绝由 `kernel` 编排、`work` 执行 |
| 派发屏障 | §4.3 | `capability/`/`policy/`、`request/`、`binding/`、`resource/`、`state/` | `work`；许可由 `resources` 签发，持久机制由 `durability` 提供 |
| 预算不超、不双重结算 | §2.2 | `budget/` | `resources` |
| 执行许可失效 | §2.10 | `resource/`、`policy/` | `resources`、`authority` |

在原有目录下，前六项不变量各需 3 个或以上目录共同写入，并有两项状态（命令幂等映射、LogicalCall 身份）没有属主。在候选划分下，每项不变量最多两个写入方，且属主唯一。结论：满足采纳条件。

## 决策

Core 由以下八个模块组成。模块内部可按实体分文件（例如 `work/` 内的 goal、task、call、action_record、subscription），但文件划分不构成模块边界。

| 模块 | 负责的不变量 |
|---|---|
| `kernel` | Kernel 生命周期、step、事件入口与完成槽、受管入口（解码 → 校验 → 分派）、命令幂等映射、跨模块编排、恢复编排、输出门控、trace 与脱敏诊断 |
| `work` | AgentInstance 生命周期与资源归属；Goal、Task、步骤与检查点；LogicalCall 与请求尝试；ActionRecord 与派发屏障；过程、会话与订阅的生命周期 |
| `authority` | 主体、节点与实例身份及其绑定证据；Session 信任级别；Grant、撤销与受限委派；Scene；包络静态准入判定 |
| `resources` | 准入、队列与冲突；租期与隔离；执行许可的签发、续租与失效；预算层级、预留与结算；优先级与公平 |
| `knowledge` | Observation 来源与有效期；记忆范围与版本；上下文预算；lineage、tombstone 与依赖失效 |
| `catalog` | 设备、组件、资源、能力、绑定、目标类型与策略的版本化描述；可信注册；兼容矩阵；注册的纯函数校验器 |
| `durability` | 持久前缀、日志与快照、已提交批次重放、迁移机制；不含领域策略 |
| `foundation` | ID、时间、状态码与原因、有界容器、通用代际栅栏 |

依赖方向为 `kernel → work → {authority, resources, knowledge} → catalog → durability → foundation`。`authority`、`resources`、`knowledge` 之间不互相依赖；需要多方参与的状态转换由 `kernel` 在同一 transition 中编排。具身包络等类型相关的校验器以纯函数注册到 `catalog`，Core 不按机器人类型编译。

## 后果

- 修改副作用类别或屏障规则时，改动集中在 `work`，必要时加上 `authority` 中的一项判定；原有目录下需要同时修改 5 个目录。
- 更换存储 Host 只改持久层的 Host 端口；新增机器人类型不改 Core。两种划分在这两个场景下相同。
- 模块较大，需要依靠内部文件结构保持可读。

## 备选方案

- **保留原有 19 个实体目录，只约定唯一属主**：评估中有六项不变量仍需 3 个以上目录写入，属主约定无法由依赖检查自动保证，否决。
- **按调用流程分层（入口 → 校验 → 执行 → 持久）**：同一实体的不变量会散落在多个层次，否决。

## 验证

实现开始后，在 CI 中用构建目标依赖检查强制上述依赖方向，禁止依赖环以及 `authority`、`resources`、`knowledge` 之间的横向依赖。
