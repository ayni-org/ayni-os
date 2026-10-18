# ADR-0008：Core 模块按不变量属主划分

- **状态**：提议
- **决定时点**：首个 Core 实现提交之前接受、修订或否决
- **日期**：2026-10-18
- **关联**：计划 §2.0、§2.3、§8；`MA-017`

## 背景

计划 §8 按实体名词把 Core 切分为 19 个目录。派发屏障这一关键安全链路分布在 `capability/`、`policy/`、`request/`、`binding/` 与 `state/`；LogicalCall 在 v1.3 中没有归属；架构图中的 Core 分组也与目录划分不一致。v1.4 已在现有目录内为每项不变量指定唯一属主（§2.3），恢复编排放在系统调用分派层，作为过渡方案。

## 候选决策

按“谁维护不变量”划分 Core 模块，实体文件保留在模块内部：

| 模块 | 负责的不变量 |
|---|---|
| `foundation` | ID、时间、状态码与原因、有界容器、通用代际栅栏 |
| `durability` | transition 日志、快照、`durable_seq`、重放、迁移机制，不含领域策略 |
| `catalog` | 设备、组件、资源、能力、绑定与目标类型的版本化描述，以及兼容矩阵 |
| `authority` | 主体信任级别、Grant、Scene、撤销栅栏、包络静态准入判定 |
| `resources` | 准入、租约与执行许可、隔离、预算层级（预留与结算）、公平性 |
| `knowledge` | Observation、Memory、Context 预算、lineage 与 tombstone |
| `work` | Goal、Task、步骤、LogicalCall、ActionRecord、过程/会话：完整的调用生命周期与派发屏障 |
| `kernel` | 生命周期、step、受管入口、跨模块编排、恢复编排、输出门控 |

依赖方向为 `kernel → work → {authority, resources, knowledge} → catalog → durability → foundation`。删除、撤销等跨模块失效由 `kernel` 编排；具身校验器以纯函数注册到 `catalog`，Core 不按机器人类型编译。

## 变更场景对比

| 场景 | 现有目录 | 候选划分 |
|---|---|---|
| 修改副作用类别或屏障规则 | `capability/`、`policy/`、`request/`、`binding/`、`state/` | `work`，必要时加 `authority` 中的一项判定 |
| 更换存储 Host | 只改持久层 Host 端口 | 同左 |
| 新增一种机器人类型 | Core 不改 | Core 不改 |

## 取舍

候选划分的模块更大，需要靠内部文件结构保持可读；采纳后需一次性迁移计划 §2.3、§8 与架构图的表述。若不采纳，现有目录必须保持 §2.3 的唯一属主约定，并由依赖检查防止横向修改。

## 决定依据

以 §2.8 的每个提交批次和 §4.3 的动作提交顺序为样例，列出每项不变量的写入方。若在现有目录下仍有不变量需要 3 个或以上目录共同写入，则采纳候选划分；否则保留现有目录并记录理由。

## 验证

采纳后，在 CI 中用构建目标依赖检查禁止反向依赖与依赖环。
