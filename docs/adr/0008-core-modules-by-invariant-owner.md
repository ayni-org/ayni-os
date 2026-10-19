# ADR-0008：Core 模块按不变量属主划分

- **状态**：已接受（设计基线，待实现与验证）；2026-10-19 经两轮对抗式评审修订
- **日期**：2026-10-18（初版）、2026-10-19（修订）
- **关联**：计划 §2.0、§2.3、§2.4、§2.8、§4.3、§8；`MA-017`

## 背景

v1.3 按实体名词把 Core 平铺为 19 个目录。提交 `48b5fae` 保留这些目录，并在 §2.3 为每行指定唯一属主，但仍有三处缝隙：

- 恢复编排的属主“系统调用分派层”在目录中没有落点；
- 命令幂等映射与 LogicalCall 身份没有属主；
- 动作账本与 Schema、Grant 同在 `capability/`、`policy/` 一行，三者的变更理由各不相同。

初版本 ADR 改为 8 个模块。随后用户反馈新结构“不如之前直观”，于是安排两位评审分别为平铺结构与 8 模块结构辩护，各写开场论证和一轮反驳。本修订记录评审结论。

**初版评估表的更正**：初版称“原有目录下前六项不变量各需 3 个或以上目录共同写入”，这一说法夸大了。原因有三：

- 旧侧按目录计数，而 `48b5fae` 的属主单位是 §2.3 的行；
- 旧侧计入了批次参与者，新侧却把同批的预算、许可、持久层记为“独立保证”，口径不对称；
- 同一提交批次涉及多个目录，并不等于同一不变量被多个目录写入。

下表改用统一口径重算。

## 评估（统一口径：只计“保证该不变量所须持有写权限的模块”）

| 不变量 | 来源 | A：平铺（`48b5fae`） | A′：平铺加 `kernel/`、`call/` | 采纳的划分 |
|---|---|---|---|---|
| 命令重试返回原对象 | §2.8 批次 1 | 幂等映射无属主；`goal/`/`task/` | `kernel/`、`goal/`/`task/` | `kernel`、`work` |
| 触发不漏不重 | §2.8 批次 2 | `goal/`、`task/` | `goal/`、`task/` | `work` |
| 步骤调用身份稳定 | §2.8 批次 3 | `task/`、`capability/`/`policy/`；LogicalCall 无属主 | `task/`、`call/` | `work` |
| 结果只应用一次 | §2.8 批次 4 | `request/`、`task/`、`capability/`/`policy/` | `call/`、`task/` | `work` |
| 删除与撤销不复活 | §2.8 批次 5 | `memory/`、`context/`、`request/` | `memory/`、`context/`、`call/` | `knowledge`、`work` |
| 派发屏障 | §4.3 | `capability/`/`policy/`、`request/`；输出门控无属主 | `call/`、`kernel/` | `work`、`kernel` |
| 预算不超、不双重结算 | §2.2 | `budget/` | `budget/` | `budget` |
| 执行许可失效 | §2.10 | `resource/`、`policy/` | `resource/`、`policy/` | `resources`、`authority` |

结论：

- **A 的缺口确凿**：有三项状态没有属主，“结果只应用一次”需要 3 个目录共同写入。
- **A′ 与采纳方案在计数上接近**：两者在派发屏障、预算、许可上持平，差距集中在 Task 与调用之间。§2.8 批次 3 中“步骤检查点 + 调用身份”是同一不变量；`blocked_on` 由待决调用派生（§2.4）；屏障后复核还要读取 Task（§4.3）。A′ 把这些分在 `task/` 与 `call/` 两处，二者要么双向依赖，要么全靠 `kernel/` 转发。
- **计数之外的决定性差别**：A′ 约有 20 个构建目标，需要逐对写出横向依赖规则；采纳方案的顶层只有 9 个节点，依赖关系现在就能写进 CI 检查。这只说明依赖规则更易于自动化，不证明构建复杂度会因此降低，后者须在实现后评估。按实体直接找到代码这一 A′ 的长处，由第二级子目录保留。

## 决策

```text
core/
├── kernel/
├── work/{agent,goal,task,call,action,session,subscription}/
├── authority/{principal,grant,scene}/
├── resources/{admission,lease,permit}/
├── budget/
├── knowledge/{observation,memory,context,lineage}/
├── catalog/{device,capability,binding,goal_type,strategy}/
├── durability/
└── foundation/
```

- **第一级是模块边界**，由构建目标与 CI 依赖检查强制。依赖方向为 `kernel → work → {authority, resources, budget, knowledge} → catalog → durability → foundation`，同层四个模块之间禁止互相依赖。
- **第二级按实体命名，用于查找。** 子目录之间的单向 include 规则在模块内声明并检查。`kernel/`、`budget/`、`durability/`、`foundation/` 职责集中，不再细分。
- **预算独立成 `budget/` 模块**，不放在 `resources/` 中，理由有三：
  - 变更理由不同：一边是计费配额，一边是 §2.10 的停止上限；
  - 恢复规则相反：已花的额度重启后不清零，许可则跨启动代际失效；
  - 保证等级不同：许可属于安全关键代码，分支覆盖率要求 ≥90%。

  资源与预算的联合准入由 `kernel` 在同一 transition 中先判定、后应用。
- **trace 记录类型与注入式 sink 放在 `foundation`**，所有模块都可以记录 trace；脱敏与导出留在 `kernel`。
- **设备与能力的描述同在 `catalog/`**，分设 `device/`、`capability/` 子目录。Binding 同时引用二者，注册时须交叉核对类型、Schema、模态、资源及取消/查询特性（§2.7），二者也共用一个兼容矩阵。运行时状态分属不同模块：在线、占用、租期与许可归 `resources/`，Grant 归 `authority/`，调用实例归 `work/`。

**命名取舍**：

- **保留 `kernel`**：Linux 源码树中也有 `kernel/` 子目录，“Kernel 内的 kernel 模块”有先例。`coordinator` 会与应用层的助理协调器（Application coordinator）撞名，`dispatch` 会与 `DISPATCHING` 和“派发屏障”撞名。
- **保留 `work`**：`execution` 会与“执行端”“执行许可”撞名。可发现性由第二级子目录解决。

## 后果

- 修改调用、动作与屏障语义时，改动集中在 `work/`，可能附带 `kernel/` 的输出门控；修改计费不影响许可代码，反之亦然。
- 按名词查找只比平铺结构多一级目录，例如 `core/work/task/`。
- `kernel/` 与 `work/` 仍有变成大模块的风险。评审时应检查二者是否开始承担其他模块的不变量。

## 备选方案

- **A：平铺 19 个目录并约定唯一属主**：有三项状态没有属主，横向依赖没有规则，否决。
- **A′：平铺并补 `kernel/`、`call/`**：可发现性最好，但 Task 与调用之间的同一不变量被拆在两个目录，约 20 个构建目标需要逐对定义依赖规则，否决。
- **初版 8 模块（单级目录）**：依赖检查简单，但一级名称不直观，`resources/` 混合了计费与安全两种风险等级，trace 放在最上层使下层无法调用，被本修订取代。
- **改名为 `dispatch/`、`execution/`**：会与既有术语撞名，否决。

## 验证

实现开始后，在 CI 中用构建目标依赖检查强制上述方向，禁止依赖环与同层模块间的横向依赖，并检查各模块内声明的子目录 include 规则。许可相关代码与预算代码分别统计覆盖率，许可适用安全关键分支 ≥90% 的门槛。
