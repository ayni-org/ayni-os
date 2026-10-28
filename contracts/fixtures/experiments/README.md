# 完整实验运行 profile

- **关联**：ADR-0075；计划 §10；[本轮实施顺序](../../../reports/core/m1-experiment-plan.md)

`fixture_schema=1` / `profile=m1-experiment` 是独立于 P6 `m1-normal` 的受控故障运行格式。JSON 在构建时校验并编译成顺序 C++ 调用，Core 和运行中的 simulator 不解析外部脚本。原 schema 0 仍是历史完整草案，不直接接受；本 profile 只接受已经实现的闭集动作。

当前六个 `HARNESS.S1.*` 用例验证运行设施：未提交创建丢失、已持久创建保留、UNKNOWN 已存写入重试、外部效果与丢失回执跨重启保留、完成结果持久后不重复执行、缺失记录与权威未执行的区分。它们不代表 E1～E4 任一完整实验通过；对应完整用例在后续阶段加入。

## 数据、控制与边界

每个文件包含 `fixture_schema`、`profile`、`case_id`、`experiment_step`、`clauses`、`script`。case_id 与文件名一致：`E2.F0.prepare_torn` 等原步骤名；`HARNESS.S1.*` 专用于运行设施自验证。完整实验步骤必须在计划存在，条款必须已定义。每文件 ≤64 KiB、至多 128 动作、单动作等待 ≤1000 step、正常推进与关闭分别 ≤2000 step、轨迹 ≤20000 条；失败不会提高上限继续跑。

| 动作 | 控制内容 |
|---|---|
| grant | 经管理意图授权本地测试计数能力，并等待正常持久推进 |
| create | 创建 action 或无触发 folder，显式非零 command_id |
| steps | 推进明确次数，storage 为 complete 或 held；held 不完成待决存储 |
| until / expect | 按闭集 metric 等待或即时断言，枚举值必须合法，计数用十进制字符串 |
| fault | 给模拟 Host 的下一次指定端口请求注入已有 StorageFault；faults_applied 只计实际匹配的请求 |
| request_query / answer_query | 管理端对原 operation 发起查询；外部计数决定 FOUND，仍在途或已执行时拒绝伪造 NOT_ACCEPTED |
| execute / receipt | 外部计数副作用与 Provider 回执独立控制；接受请求不执行副作用，重启不重置计数 |
| crash / start | 先停止同步模拟生产者并丢弃旧 completion 指针，Host 结束旧存储借用，再丢弃 arena；以同一 Host 镜像重建 |

配置由本地模拟 profile 显式选定并完整记录；种子为 0，时钟只按脚本顺序推进，模型与真实设备均未接入。运行器不自动执行、取消或重试未知动作。测试服务不按 operation 去重，重复 execute 会使独立计数增加，防止服务自身掩盖 Core 重复派发。

## 运行与证据

构建前按开发指南检查资源、设置正整数构建和测试并行度。`ayni-sim --list-experiments` 只列实际编译的用例；`--case HARNESS.S1.external_effect_survives_cut --output NEW_DIRECTORY` 执行单例；`--experiment HARNESS --output NEW_DIRECTORY` 执行本阶段自验证。尚无用例的 E1/E2/E3/E4 不会空跑成功。

每例写出 `run.json` 与 `trace.jsonl`：实际源码修订、工具链、配置、ABI/schema、注入/重启、命令提交身份、公开 Goal/Task/Call 变化、独立效果计数、断言及首个失败。它是逐 step 公开观察与外部设施轨迹，不是内部 journal 每条 transition 的镜像；记录不包含业务正文、密钥或媒体。输出目录须新建，拒绝覆盖已有证据。主失败、关闭失败与产物失败分别报告，任一失败 CLI 非零退出。

| 条款 | 约束 | 反例 | 验证 |
|---|---|---|---|
| C-EXP-001 | 完整 profile 在构建时拒绝未知/重复字段、未实现动作、无效步骤/条款、非法数值及超限；不能将正常 profile 或草案冒充完整实验 | storage_fault 被忽略、零断言脚本报告通过 | `tests/tools/test_generate_experiment_fixtures.py`；完整实验覆盖仍按各后续阶段报告 |
| C-EXP-002 | 故障和重启作用于真实模拟 Host/Kernel；旧生产者先结束借用，持久镜像与外部效果独立保留 | 仅改查询结果变量假装断电，或沿用已释放的 completion | `ExperimentFixtures.compiled_scenario_records_actual_fault_and_assertion_results` 的 HARNESS.S1 六场景 |
| C-EXP-003 | 副作用执行与回执分别控制；独立计数跨 Kernel 重启保留，测试服务不代替 Core 去重 | 每次重启清零计数而掩盖动作重复 | HARNESS.S1.external_effect_survives_cut、HARNESS.S1.completed_receipt_survives_cut |
| C-EXP-004 | 首个失败不被后续匹配检查覆盖，关闭/产物错误仍导致失败；现有目录和未知/空实验拒绝 | 失败后清理返回 OK 把整例改成通过 | `ExperimentEvidence.first_failure_survives_later_success_and_retains_classified_status`；`ExperimentEvidence.unregistered_case_is_rejected_before_creating_output`；`ExperimentEvidence.rejected_recovery_fails_before_empty_world_assertions`；tests/integration/experiment_artifacts_test.py |
| C-EXP-005 | CLI 与 GoogleTest 共用编译脚本及执行器；相同输入两次逻辑证据一致，版本/配置可查 | CLI 只打印预置“通过”，或分别维护两套恢复逻辑 | tests/integration/experiment_artifacts_test.py；`ExperimentFixtures.compiled_scenario_records_actual_fault_and_assertion_results`；`ExperimentEvidence.core_and_provider_clock_stay_equal_with_storage_held` |
