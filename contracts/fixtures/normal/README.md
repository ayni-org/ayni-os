# P6 正常集成夹具

- **关联**：ADR-0068、ADR-0012；P6；计划 §10.1～10.5

`fixture_schema: 1` / `profile: m1-normal` 是已实现正常闭集。原 [完整草案](../README.md) 的 schema 0 与故障动作保持未交付；本目录不接受它们。64 位数值采用十进制字符串，种子固定为 `"0"`，场景由显式动作顺序决定。JSON 在构建时生成 C++ 数据，运行时不能加载外部脚本。

| 编号 | 本轮证明的正常范围 |
|---|---|
| E1-N1 | 私有读、明确共享、撤销后拒读；读取者是 Task 所有 Agent |
| E1-N2 | 单模型槽有等待时，本地事件先完成；记录含持久推进的实际步数 |
| E1-N3 | 两次启动硬上限阻止第三次模型调用，另一 Agent 继续 |
| E1-N4 | 暂停/恢复的当前 revision、跳过自主积压、下一周期只执行一次 |
| E1-N5 | 两个 Agent 在共享摄像头容量内同时观察，模型仍串行 |
| E2-N1 | 普通持久屏障之后一次计数副作用，原命令正常重试返回原 Goal |
| E3-N1 | 静止检查点正常关闭/重开、兼容策略和 Mock Binding 切换；保留原身份/步骤/计数 |
| E4-N1 | Task 订阅、实际 Mock 样本分析、停止确认、模拟呈现成功后完成 |
| E4-N2 | 自主暂停期间提醒继续、健康检查不执行；正常重开不重播已呈现提醒 |

## 数据与动作

每个 `<case_id>.json` 必须包含 schema/profile、case_id、experiment_step、clauses、description、seed、config、script、expect。字段、枚举和上限由 `tools/generate_normal_fixtures.py` 校验；case_id 不复用完整实验编号，引用的条款和步骤必须已定义。`config` 明确 tick_us、model_delay_us、step_units、max_steps、model_hard_limit；其余实际 Kernel 配置随产物完整保存。

动作是 grant、save_memory、share_memory、revoke、goal、steps、advance_clock、set_wall_clock、wait、assert、pause_group、resume_group、pause_goal、resume_goal、local_input、retry、reopen。每个新对象有唯一名称；引用须已创建且类型匹配。local_input 只接受 local Goal，以其唯一名称作为输入源。device_check 固定组 1，reminder 固定组 0；错误配置在生成期拒绝。wait 有 max_steps 上限，assert 只检查当前事实，不推进到断言成立。

断言的 metric 枚举覆盖公开目标/任务状态与 hold、步骤/模型次数、实际执行计数、资源最大占用、已停止订阅、屏障检查及重开保留身份。比较是 equal、at_most、at_least。状态/原因使用已列出的符号名，其他值用非负十进制字符串。主失败、close_status、artifact_status 分别报告，不能把失败当成功或清除首因。

## 产物与证据

`ayni-sim --list-normal` 列出编译用例；`ayni-sim --case E1-N1 --output NEW_DIRECTORY` 重放一例。目录必须尚不存在；已有路径拒绝，避免覆盖历史证据。无参数仍运行原 Native 生命周期演示。

`run.json` 记录代码修订/dirty、编译器/build_type/sanitizer、ABI/storage/尚未实现 wire 的版本边界、Kernel 配置、模拟参数、arena 与结果。`trace.jsonl` 记录步后观察到的公共状态变化和独立执行计数。它不是全部 journal transition 的导出；`barrier_checks` 仅检查模拟副作用发生前的持久事实，不取证 submit 入口或故障窗口。模拟呈现是本地计数器渠道，不证明 CLI、真实屏幕或用户已读。

重开只接受无在途、无未知效果 Call 的静止点；不注入故障、不丢弃易失数据来构造异常路径。原完整实验、恢复竞态、硬件停止时限与 Core Alpha 均未据此验收。

## 契约

| 编号 | 规范 | 反例 | 验证 |
|---|---|---|---|
| C-FIX-001 | 正常数据在构建时验证并编译为只读表；未知字段/动作、错误引用、重复键、超限及非法数值明确拒绝 | storage_fault 被静默忽略后报告正常通过 | tests/tools/test_generate_normal_fixtures.py；`NormalFixtures.compiled_normal_scenario_records_successful_execution` |
| C-FIX-002 | CLI 与 GoogleTest 共用执行器；有界假时钟、等待和产物量，固定顺序两次重放轨迹一致 | 框架各写一份领域状态机；依赖真实模型输出巧合相同 | `NormalFixtures.compiled_normal_scenario_records_successful_execution`；tests/integration/normal_artifacts_test.py |
| C-FIX-003 | 运行事实只经公开 API 查询/意图取得；模拟副作用有独立执行计数；接受不算执行，回执背压不重执行 | 用 Call 已完成的日志代替设备侧计数器 | E2-N1、E4-N1；`NormalFixtures.compiled_normal_scenario_records_successful_execution`；Counter 可靠回执分支静态审查，未新增故障专项 |
| C-FIX-004 | 重开先拒绝在途/未知效果，在静止点正常排空；保留 Task/owner/Call/operation/模型次数和步骤；不替换旧已接受身份 | 新建一个 Task 假装从旧检查点继续 | E3-N1、E4-N2；`NormalFixtures.compiled_normal_scenario_records_successful_execution`；原恢复矩阵仍未验证 |
| C-FIX-005 | run/trace 不含私有正文、凭据或媒体；元数据明确正常范围及硬件/专项未验证；版本和实际配置可复查 | trace 打印 memory.read 正文，或把模拟结果写成实机 E1–E4 通过 | tests/integration/normal_artifacts_test.py 的实际 JSON/合成正文检查与双次重放；实际结果在 PR |
| C-FIX-006 | 首个失败与 close/artifact 分类独立；不能确认关闭时保留整个组合，包含外部注册上下文 | 清理抛异常越过借用保留，先析构 policy 再销毁 Kernel | 正常关闭由九个场景验证；异常及保留边界为独立静态审查，未新增故障/恢复专项 |
