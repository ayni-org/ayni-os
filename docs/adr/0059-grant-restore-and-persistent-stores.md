# ADR-0059：Grant 恢复职责与持久存储适配

- **状态**：已接受，P3 前置整改已实现、审查中
- **日期**：2026-10-27
- **关联**：P3 F12、F04 的 Grant/命令部分；ADR-0030、0031、0051、0058；C-AUTH-023、C-AUTH-027、C-STORE-015、C-STORE-017

## 背景

记忆共享与删除要复用撤销闸门、精确命令和持久实体契约。GrantTable 原来同时保存事实、易失拒绝集合并校验恢复历史；Grant/命令仍由 journal/snapshot 协调器直接编码。先以独立 PR 完成这两个既有边界的整改，再审查记忆及隐私清理，P3 共三个 PR，仍在已定范围内。

## 决策

1. **事实、控制与历史证明。** GrantTable 保持事实表和既有内部入口；RevocationGate 独立拥有有界的已接受拒绝 ID 集合，clear_restored 只丢事实，完整 clear 才丢闸门。GrantRestore 是同一 Grant 模块的恢复侧属主，负责历史关系、精确替换位置和记录恢复；通过窄友元访问同一份表内存，无第二份事实或持久缓存。既有入口转交该属主，避免改变消费者契约。live/restore 的终态写入和两轴时间戳规则只在 grant_state.h 定义一次。
2. **沿用 PersistentStore。** Grant 的创建/状态/根撤销/清理进度属于一个记录族，command 属于另一个。描述符声明族内类型、版本/上下文、REQUIRED 起点、原有完整 journal 字节预算、快照容量/最大记录及遍历。Call/Scene 继续使用同一契约；没有运行期注册或第二套持久化系统。
3. **保留顺序与配对。** journal 依然按 binding/instance、Grant 子序列、Goal/Task、Call/Scene、command、cleanup footer 编码；snapshot 依然 Grant 在工作之前、Call/Scene 在 Task 之后、command 在 cleanup 之后。Grant 先父后子，命令按原接受 commit_seq。WorkReplay 继续负责跨记录配对与批次完整性，不把这些规则下放给孤立表。
4. **有界游标与单一事实。** 每个适配器选择下一行，整条记录写入后才推进；页面放不下时重选同一行。StoreCursor 的 index 是表行，order 是该适配器的深度或已写命令序号，切换适配器即归零，不跨快照保存。Grant/Goal 共用原有有界深度遍历。命令 reader 复用 CommandTable 已有的最后成功加入序号，删去协调器重复缓存，保留常数时间的顺序检查。
5. **兼容。** 行为、ABI 0.39、schema 2、记录版本与字节布局不变；旧 command v1 保持 optional，Grant/Call/Scene 的未知版本（含 0）仍要求 REQUIRED。限额公式迁到适配器，保留原有保守启动过期批次预算。内部对象布局可能变化，按公开内存查询量测，不把布局当 ABI。
6. **扩展场景。** 后续 Memory 只需自己的记录族/适配器与一个编译期描述符条目；跨 Task、调用和命令的 lineage 配对仍改 Kernel 编排。更换存储 Host 不影响这些事实/codec，也不要求改 GrantRestore。

## 后果

- 授权共享可以复用独立的易失拒绝机制；重载恢复不会清除已接受的拒绝。
- journal/snapshot 协调器不再知道 Grant/command 的记录布局与版本；恢复跨实体关系仍可整体审查。
- 保留少量内部转交入口，维持既有消费者稳定；不把每个表操作包装成新层。
- F04 的 Goal/Task/Agent 迁移仍按计划随 P5，F10 的新记忆入口解码随 MA-011A。

## 备选方案

- 只移动 grant_table.cpp 内的方法：仍混合状态属主和恢复规则，不采用。
- 每个 Grant 记录类型独立成一张存储表：会分散根撤销、状态与清理进度的固定顺序，采用单一记录族。
- 改写 schema 或统一所有记录体：这次无产品需要，兼容成本过高，不采用。
- 新建持久注册框架：已有 PersistentStore 可扩展到真实的多类型、顺序差异，无需并行机制。

## 验证

相关既有正常功能、记录与快照测试通过；独立审查与最终 CI 结果将在 PR 集中记录。不新增故障/恢复专项，也不据此声明记忆删除、E1–E4 或硬件验收完成。
