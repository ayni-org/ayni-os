# ADR-0036：未确认授权的 Goal 普通控制守卫

- **状态**：已接受。
- **日期**：2026-10-23
- **关联**：`MA-006` R3；复查 A1；ADR-0028 决策 4；C-LIFE-021；[实施计划](../ma006-implementation.md) §4。

## 背景

恢复的活 Goal 已经以 `LEGACY_UNCONFIRMED` 阻止工作推进，查询显示有效 PAUSED，但普通暂停/恢复仍按保存的生命周期判断。底层 ACTIVE 的 Goal 会接受普通暂停，底层用户 PAUSED 的 Goal 会接受普通恢复，从而修改本应保留的生命周期与 revision。原有测试没有覆盖这两个控制入口。

## 决策

1. 在状态转换属主 `GoalTable::allows()` 中统一拒绝授权未确认时的普通 pause/resume。落实 ADR-0028 既有规则，不改变暂停原因、不把拒绝覆盖写成持久批准，也不在 Kernel 重复校验。
2. 保持原错误顺序：expected revision 冲突先于状态拒绝，状态拒绝先于 revision 耗尽。拒绝不产生 transition，不改 Goal、Task 或其检查点；取消/失效及其他兼容暂停原因保留既有语义。
3. 先实际运行经 C ABI 的失败测试，再修改生产判断。分别覆盖恢复前 ACTIVE 与用户 PAUSED，核对 revision、commit、occurrence、Task 引用不变，并验证 stale revision 与取消仍遵守原契约。
4. 无新状态、ABI 或持久格式。明确授权并恢复仍由 E/F 交付，本修复不使恢复工作自动运行。

## 后果

Goal 与 Task 的普通恢复均不能解除授权未确认。更换 Host 不影响此规则；恢复持久事实与展示有效状态仍分别由原属主负责。

## 备选方案

- 在 API 入口按查询到的 PAUSED 拒绝：会把其他暂停原因混为一谈，并绕过 GoalTable 的状态规则，拒绝。
- 把恢复的存储 lifecycle 改写为 PAUSED：会丢失用户是否暂停的事实，且不符合 ADR-0028，拒绝。

## 验证

新增两项 C ABI 场景在修改生产代码前均实际失败，符合 A1 的两个触发条件。修复后 GCC Debug、Release、ASan/UBSan、TSan 和 Clang Debug 全部通过；模拟器、工具/依赖/契约/可读性/格式检查及 ESP-IDF v6.0.3 固件编译通过。默认与最小工作容量的 arena 内存需求未变。

独立会话提出 15 项植入缺陷，首轮检出 11 项；补测后其余 4 项全部检出。补测验证兼容 hold、终态准入、错误优先顺序及真实恢复 Task/非空检查点不变。独立审查无未关闭问题。命令、逐项结果、四项可读性审查及验证限制见 [#40](https://github.com/RobotSe7en/ayni/pull/40)。本轮按用户授权仅做本地 CI；覆盖率在本部分合并后统一补记，不表示 E/F 或硬件已完成。

### R3 合并后的覆盖率

GCC 13 Debug 插桩完整测试通过，收集全部 Core 及头文件消费者并沿用 R2b 的合并口径。Core 含头文件行覆盖率 97.73%、分支 91.01%；GoalTable 属主含头文件为 98.24%/93.48%；R3 修改的可执行行及分支均为 100%。达到 Core 行及本次安全状态判断的分支目标。分子分母与方法见 [#40](https://github.com/RobotSe7en/ayni/pull/40)，不表示硬件或 E/F 已验证。
