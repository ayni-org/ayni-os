# ADR-0041：Task 记录编码归属

- **状态**：实施中
- **日期**：2026-10-24
- **关联**：MA-010；C-VER-023、C-STORE-014；计划 §9.1

## 背景

work/task 已拥有 Task journal 记录，但 Kernel 快照重复编写单步骤记录与 Task 快照历史尾部。记录长度计算也分散于 journal 和 snapshot，格式演进需修改多份布局知识。

## 决策

1. TaskStepRange 明确选定连续步骤区间；Task codec 的同一尺寸函数与写入器供 journal 增量及 snapshot 单键调用。调用者从已验证的 Task 选择合法范围，不增加布尔模式或动态接口。
2. TaskSnapshotRecord、快照固定长度及双向 codec 归 work/task；journal 创建体继续复用原编码。Task 字段含义及恢复不变量仍由 TaskTable 维护。
3. Kernel 只选择记录、分页、安排先步骤后最终状态的顺序，并检查快照全局 stamp 上限和 Goal/owner 关系；不再解释 Task 尾部字段布局。Kernel 最大容量计算调用 Task codec 的最大尺寸函数。
4. 保持 snapshot TASK、TASK_STEPS、checkpoint 的字节、版本、错误分类及容量结果。无新增公开符号、Kernel 字段或平台依赖；ABI 保持同一 PR 前置的 0.28，schema 仍为 2，无迁移。

## 后果

新增 Task 存储字段时，布局只在属主模块改变；新的快照容器继续通过记录接口组合。区间 API 为内部预条件接口，不接受未验证外部范围。创建 stamp 与最后 commit sequence 的原有保存语义保持一致。

## 备选方案

不保留 Kernel 的独立单步骤写入器，避免同格式两份实现；不建立通用序列化框架，因为现有 BodyReader/BodyWriter 已满足需要；不把分页和跨表校验移入 Task codec，以保留状态与容器职责边界。

## 验证

新增固定字节向量覆盖 Task 快照尾部和历史单步骤切片，保留既有分页恢复及 journal 场景。按用户 2026-10-24 指示，本次与批次身份修复合为同一 PR，不再做独立审查及植入缺陷测试；作者自查及必要验证结果完成后补入。
