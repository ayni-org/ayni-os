# ADR-0078：完整 E3 重启后替换模型与兼容策略

- **状态**：已接受；交付结果见本阶段 PR
- **日期**：2026-10-28
- **关联**：MA-023、计划 §10.4；ADR-0026、0045、0056、0075～0077

## 背景

正常 E3 只在所有调用结束后重开，不能证明旧模型仍在途时的身份与隔离，也没有不兼容私有检查点的完整实验。用户已授权按 E2→E1→E3→E4 完成 Native 确定性实验；本阶段不接入真实模型或硬件。

## 决策

1. **共同进度与实现。** 应用夹具声明一个有限流程：计数动作 `increment`、共同步骤 `checkpoint.ready`、模型步骤 `round.0.model.v1`。规则测试策略按共同调用结果推进；runtime 装配真实 ToolLoop，并复用应用拥有的共同前置步骤。策略选择、Conversation 格式校验、具体 Provider 与生命周期都由 runtime 负责；应用不依赖具体策略库。
2. **检查点边界。** 规则只写带原 writer ID/版本的空私有状态；共同步骤及调用由 Core 保存。ToolLoop 显式声明接受规则 writer 的 v1 空格式，随后写自己的真实私有格式。不声明规则能读取 ToolLoop 私有字节；切为规则或未声明兼容的 ToolLoop v2 时，恢复须以 `UNSUPPORTED_VERSION / STRATEGY_CHANGED` 暂停，不改原字节、writer、步骤、revision 或消耗。
3. **重启基线。** 在计数动作完成并持久后暂停同一 Task；另一个 Task 保留实际 Mock A 模型请求。基线保存 Agent instance、Goal/Task、Grant、私有 Memory、调用/operation/attempt、共同步骤、版本和模型消耗。模拟断电先结束旧生产者与 Host 借用，再销毁旧 Kernel；外部效果记录独立保留。
4. **旧请求与新绑定。** 停机时将模型 Binding 的本次目录选择改为 B。恢复出的旧 Call 保留已接受的完整 A 路由与身份，可能派发的旧请求归为 UNKNOWN；Mock 无权威查询能力时保持未知。只有新独立 Call 使用 B。将复制的旧 A attempt 投给当前 B completion，验证拒绝且两项调用均不被旧包完成；不向旧 arena 投递。
5. **只读证据。** 已公开的生命周期、消耗、Memory/Grant 和结果查询均走 C ABI；未公开的私有检查点、步骤键和不可变调用路由由 `tests/experiments/progress_evidence.cpp` 经已有 `KernelTestProbe` 只读复制。诊断副本不驱动策略、授权或派发。探针关闭时返回 `UNSUPPORTED`，不会用空证据冒充通过；不增加公开 ABI 或生产 Core 字段。
6. **可重放与确定性。** 原 E3 步骤 1～5 的八项清单分别编译为完整 profile。相同公开共同快照分别两次输入规则和真实 ToolLoop，比较完整请求与私有输出；CLI 再比较两轮实际轨迹。合规 writer/格式的兼容不要求两个策略输出相同正文，只要求分别确定性并保持共同身份/完成语义。

## 后果与备选方案

复用原 Call 恢复、checkpoint reader 与 Mock，不增加迁移器、重试机制或 Core 场景分支；ABI 0.45 / schema 2 不变。新模型配置不能为旧 UNKNOWN 授权换 operation 重做。测试专属只读探针使确切检查点和路由保留可核查，代价是完整 E3 运行要求启用测试。真实模型/ESP32 的步骤 6 留在 M2，模拟结果不证明硬件、媒体或耐久。

## 验证

Native/Sim 的八个 E3 场景、共同快照确定性及现有 E1/E2/HARNESS 回归与两轮 CLI 产物重放通过。具体命令、配置、独立审查、最终 CI 与限制集中本阶段 PR。E4、独立安全覆盖、Core Alpha、完整 MA-038 与硬件仍未验收；按本轮授权随后推进 E4。
