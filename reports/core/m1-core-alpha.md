# M1 Core Alpha 报告（Native 功能验收）

- **日期**：2026-10-28
- **功能基线**：安全覆盖交付 [#102](https://github.com/RobotSe7en/ayni/pull/102)，覆盖率冻结提交 `5a9998718421f6ca6198a315e75c2517fce2eed8`；实验性 C ABI 0.45 / storage schema 2
- **范围决定**：用户 2026-10-28 最新授权先完成安全覆盖、再完成本报告；本轮安全分支门槛接受 ≥80%，通用 ≥90% 偏差保留，M2 暂缓；见 [ADR-0080](../../docs/adr/0080-safety-branch-coverage-closeout.md)

## 验收结论与边界

Core Alpha 的 Native 功能证据已具备：同一便携 Core 的状态、运行限制、授权、恢复、公开 SDK 与电脑 E1～E4 均有可执行验证，本轮用户接受的安全分支 ≥80% 门槛也已达到。本文收齐证据，并保留通用 90% 目标偏差及既有存储兼容待决项；全部契约闭环、硬件验收与 v0.1 发布门槛仍不能据此宣称完成。

本报告 PR 只整理证据与状态，不改生产代码、公共 ABI、持久格式、统计口径或验收阈值，不新增测试专项，也不重新采集覆盖率。最终实现的 Linux 完整配置验证复用 #102；报告本身的检查与独立审查集中在本次交付 PR 的 Validation / Review。阶段内旧 CI 仅证明各自冻结修订。

M2、ESP-IDF 存储实现与板级运行均暂缓。完整 MA-038 仅交付过 typed 正常子集，仍不阻断 M2，但不能据此宣称生产远程协议已完成。真实模型多轮试用、硬件停止、媒体和耐久不在本报告通过范围。

## 必需能力与证据

| 验收项 | 实际交付及可核查入口 | 证据边界 |
| --- | --- | --- |
| 生命周期、状态与调度 | [契约追踪](../../contracts/traceability.md)、[Core 模块](../../core/README.md)、[E1 / ADR-0077](../../docs/adr/0077-e1-shared-service-acceptance.md)：单 owner、有界 step、可靠完成、暂停与资源全组准入 | Native/Sim；虚拟时间不是硬件实时上限 |
| 授权、隐私与运行限制 | [E1 / #99](https://github.com/RobotSe7en/ayni/pull/99)：语音信任、共享撤销/删除与晚到结果、模型次数限制、公平性、提醒呈现；[E2 / #98](https://github.com/RobotSe7en/ayni/pull/98)：撤销与独立许可门 | 可信内置代码通过受管理入口；不声明任意原生代码隔离 |
| E2 动作与提交恢复 | [ADR-0076](../../docs/adr/0076-e2-recovery-and-executor-evidence.md)、[#98](https://github.com/RobotSe7en/ayni/pull/98)：F0～F14 的切断、跨对象提交、命令重试、未知查询和许可失效 | Sim Host 持久镜像和独立 Mock 执行端；非真实 Flash/执行器 |
| E1 共享系统服务 | [ADR-0077](../../docs/adr/0077-e1-shared-service-acceptance.md)、[#99](https://github.com/RobotSe7en/ayni/pull/99)：步骤 1～11；本地规则事件 ≤10 次实际 step | 资源等待按用户决定为 30 秒，取代原 500 ms；不沿用 P6 的 30 step 作为完整 E1 门槛 |
| E3 重启替换 | [ADR-0078](../../docs/adr/0078-e3-provider-and-policy-replacement.md)、[#100](https://github.com/RobotSe7en/ayni/pull/100)：共同进度、兼容策略、更换 Mock 模型、旧调用隔离及不兼容暂停 | 不把旧 UNKNOWN 操作重绑新模型；真实模型/ESP32 复核仍待后续 |
| E4 组合与版本 | [ADR-0079](../../docs/adr/0079-e4-composite-and-version-acceptance.md)、[#101](https://github.com/RobotSe7en/ayni/pull/101)：父子组合、提交边界、依赖失效、停止/通知和兼容矩阵 | 用户选定 typed 注册验证；JSON Schema 关键字留 MA-007。开发期不做迁移器/旧格式 reader，改验不支持格式拒绝、原数据保留及禁止输出；不将原迁移项算通过 |
| C/C++ SDK 与接入示例 | [公开示例](../../examples/README.md)、[ADR-0067](../../docs/adr/0067-cpp-sdk-and-public-examples.md)：new_hardware、new_model、new_strategy、composite_workflow | Native；模型示例使用本地 HTTP 夹具，无付费服务/真实设备 |
| CLI 与助理 | [CLI 使用说明](../../targets/native_cli/README.md)、[#94](https://github.com/RobotSe7en/ayni/pull/94)、[#95](https://github.com/RobotSe7en/ayni/pull/95)：单一 ToolLoop 助理、目标待批、确定性停止与 system 时间信息 | 本机真实模型多轮试用仍未完成；旧双 Agent 目录明确拒绝，不迁移 |
| 容量与开销 | [正常集成报告](m1-normal-integration.md)、[ADR-0069](../../docs/adr/0069-native-capacity-measurements.md)、[#91](https://github.com/RobotSe7en/ayni/pull/91) | 历史 Native arena 与选定满表测量；不当作全表同时填满、WCET 或 ESP32 RAM/栈结果 |

E1～E4 的完整 profile、用例与兼容矩阵分别见 [完整夹具说明](../../contracts/fixtures/README.md)、[用例清单](../../contracts/fixtures/cases.md) 和 [E4 矩阵](../../contracts/fixtures/e4-compatibility.json)。测试开启时的精确探针复用记录属主 codec；关闭测试则明确拒绝，不用正常关闭代替断电。重放命令见 [开发指南](../../docs/development.md)。

## Linux 配置验证与来源

每阶段提交均经过独立审查、修复和该阶段最终提交的 ASan/UBSan CI；具体命令、测试数和 CI 运行入口在 [#98](https://github.com/RobotSe7en/ayni/pull/98)、[#99](https://github.com/RobotSe7en/ayni/pull/99)、[#100](https://github.com/RobotSe7en/ayni/pull/100)、[#101](https://github.com/RobotSe7en/ayni/pull/101)。最终实现已在 [#102](https://github.com/RobotSe7en/ayni/pull/102) 显式完成完整配置 CI：GCC ASan/UBSan、GCC Release、Clang Debug、GCC TSan 与已有 ESP32-S3 骨架固件编译。命令、测试数、工具链和运行入口集中 #102；本报告仅改 Markdown，复用相同实现与工具配置的结果，不把 #93 的旧结果当作 E4 后验证，也不重跑相同矩阵。

固件编译只证明已有骨架能编译，不启动 M2、不证明 ESP-IDF 存储或板级功能。原始实验轨迹、覆盖率 JSON 和本地日志未上传为仓库/CI 附件，相关 PR 已注明此来源限制；仓库中的夹具、断言、工具和 CI 可复现软件验证。

## 安全覆盖与保留偏差

[#102](https://github.com/RobotSe7en/ayni/pull/102) 对上述干净冻结修订重新构建全部目标、清除 counters 后，运行完整测试与模拟器。GCC/gcov 13.3.0、Debug、无 sanitizer、atomic counters；精确命令、结果及独立审查集中该 PR。

- Core 行覆盖 **22,310 / 23,469 = 95.06%**，达到 75% 目标。
- 保守安全分支覆盖 **19,886 / 24,020 = 82.79%**，达到用户本轮接受的 ≥80%；通用 ≥90% 未达的偏差记录于 ADR-0080。
- 范围仍为全部 `core/`、`control/permit_gate/`、`providers/mock/`、`providers/mock_execution/` 及这些目录在相应 TU 中的头文件。行并集与函数/有序布局分支槽并集，仅过滤 compiler throw arcs；未删除防御分支或缩分母，未覆盖分支不统称不可达。
- 全部安全范围实现有 gcov；未单列的声明/编译期头不据此声明运行时覆盖。原始 JSON、摘要和本地日志仅在本机 ignored `build/safety-final-*`，未作为仓库/CI 附件交付。

该补测同时修复已知停止后订阅新代的恢复，以及四个 CallRuntime 回调未来 ABI 尺寸的错误分类；ABI 0.45/schema 2 不变。历史 P6 和 E4 后测量保留原修订与数值，不改写为本轮结果。

## 既有待决兼容问题

独立审查发现 C-STORE-004 对未知非 REQUIRED 记录的跳过承诺，与独立原子批次不得含其他记录的条文冲突。真实 wait-resume journal 定位实验在批次前、中、后插入 CRC 正确的未知 optional 扩展，当前 reader 均恢复受限。详见 [ADR-0080](../../docs/adr/0080-safety-branch-coverage-closeout.md) 和 #102 Limitations。

该行为在本次覆盖 PR 前已存在。统一跳过扩展或明确原子批次兼容例外的选项已向用户提出，尚未确认；本轮没有修改语义，没有把当前拒绝固化成新契约。此项继续待决，因此本报告不声明存储兼容契约已全部闭环；定位用临时断言不作为验收通过证据。

## 交接

本轮交付止于本报告、相关状态同步及其 PR 的独立审查/修复/最终 CI/合并。M2 按用户决定暂缓，不自动开始 ESP-IDF 存储、管理协议或实机操作；完整 MA-038、真实模型多轮试用和后续平台工作分别保留原未验证边界，等待后续安排。
