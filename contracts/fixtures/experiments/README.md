# 完整实验运行 profile

- **关联**：ADR-0075～0078；计划 §10；[本轮实施顺序](../../../reports/core/m1-experiment-plan.md)

本页描述已交付的 Native/Sim profile。后续按 [ADR-0083](../../../docs/adr/0083-general-foundation-before-platform-experiments.md)先完成基础通用能力，再独立安排 ESP32-S3、Linux 等所选平台实验；本 profile 的现有结果不代表后续平台、设备或实际 OS 集成已经通过。

`fixture_schema=1` / `profile=m1-experiment` 是独立于 P6 `m1-normal` 的受控故障运行格式。JSON 在构建时校验并编译成顺序 C++ 调用，Core 和运行中的 simulator 不解析外部脚本。原 schema 0 仍是历史完整草案，不直接接受；本 profile 只接受已经实现的闭集动作。

初始六个 `HARNESS.S1.*` 用例验证运行设施：未提交创建丢失、已持久创建保留、UNKNOWN 已存写入重试、外部效果与丢失回执跨重启保留、完成结果持久后不重复执行、缺失记录与权威未执行的区分。它们不代表 E1～E4 任一完整实验通过；另有活动执行端断电后清理的 HARNESS 回归；E2 F0～F14 原清单及附加闭环场景已加入；完整 E1 的场景见 ADR-0077；E3 见 ADR-0078，E4 Native/Sim 组合与版本证据见 ADR-0079；ESP32 仍须单独复核。

## 数据、控制与边界

每个文件包含 `fixture_schema`、`profile`、`case_id`、`experiment_step`、`clauses`、`script`。case_id 与文件名一致：`E2.F0.prepare_torn` 等原步骤名；`HARNESS.S1.*` 专用于运行设施自验证。完整实验步骤必须在计划存在，条款必须已定义。每文件 ≤64 KiB、至多 128 动作、单动作等待 ≤1000 step、正常推进与关闭分别 ≤2000 step、轨迹 ≤20000 条；失败不会提高上限继续跑。

| 动作 | 控制内容 |
|---|---|
| composite | E4 有限父子组合、采样/历史分析/停止/通知、11 项提交边界和版本矩阵；属主 codec 的版本/切断探针需启用测试 |
| replacement | E3 双 Mock/规则与真实 ToolLoop 的有限重启/替换场景；只接受 replacement 部署；原进度/路由只读探针需启用测试 |
| grant | 经管理意图授权本地测试计数能力，并等待正常持久推进 |
| create | 创建 action、物理 turn 或无触发 folder，显式非零 command_id |
| steps | 推进明确次数，storage 为 complete 或 held；held 不完成待决存储 |
| until / expect | 按闭集 metric 等待或即时断言，枚举值必须合法，计数用十进制字符串 |
| fault | 给模拟 Host 的下一次指定端口请求注入已有 StorageFault；faults_applied 只计实际匹配的请求 |
| request_query / answer_query | 管理端对原 operation 发起查询；外部计数决定 FOUND，仍在途或已执行时拒绝伪造 NOT_ACCEPTED |
| execute / receipt | 外部计数副作用与 Provider 回执独立控制；接受请求不执行副作用，重启不重置计数 |
| crash / start | 先停止同步模拟生产者并丢弃旧 completion 指针，Host 结束旧存储借用，再丢弃 arena；以同一 Host 镜像重建 |

附加可选字段 `query_authority=NONE/BEST_EFFORT/AUTHORITATIVE`（默认 AUTHORITATIVE）、`deployment=counter/physical/shared/shared_physical`（默认 counter）选择有限部署并写入 run.json。E2 新动作：`budget`（1～64）、`save_call/same_call`、`retry`、`revoke/cancel_goal`、`read_result`、`conflict_command/expire_command`、`start_recovery/finish_recovery`、`fault_pending/complete_storage/complete_old_writer`、`repeat_receipt/post_old_attempt`、`application_policy`、`exhaust_storage/restore_storage`（实际 Host 容量及明确空间不足原因）。它们均通过原公开契约或模拟属主执行。

`hold_record/cut_record` 只接受固定记录边界，`damage` 只接受 torn_tail/committed；精确 codec 探针仅在测试构建提供，关闭测试明确拒绝，不近似替代。`core_now` 推进 Core 而不走时；`physical` 的闭集动作控制执行端本地周期、链路、联锁、实际许可重放及测量。`at` 按首个实际许可的 nonce 时刻定位（0～1000000 µs），不会回拨时间；恢复链路须由脚本显式指定。重放测试丢弃旧 completion 指针，只向当前有效目标投递旧身份值。

`shared` 动作仅接受已实现的有限场景名；共享配置固定模型槽 1、Task 上限 2、100 ms 正常模型、1,500/1,000 ms 超时与用户选定的 30 秒资源等待，实际值进入轨迹。Host 依据公开 step report/wake 和 Provider 截止推进 FakeClock；所有实际 Core step 均计数，本地规则必须 ≤10 step。普通满队列的 Kernel 完成与完成槽耗尽分别验证，后者复用测试专属 CompletionPool 属主探针，测试关闭时拒绝。

配置由本地模拟 profile 显式选定并完整记录；种子为 0，时钟只按脚本顺序推进，模型与真实设备均未接入。运行器不自动执行、取消或重试未知动作。测试服务不按 operation 去重，重复 execute 会使独立计数增加，防止服务自身掩盖 Core 重复派发。

## 运行与证据

构建前按开发指南检查资源、设置正整数构建和测试并行度。`ayni-sim --list-experiments` 只列实际编译的用例；`--case HARNESS.S1.external_effect_survives_cut --output NEW_DIRECTORY` 执行单例；`--experiment HARNESS --output NEW_DIRECTORY` 执行设施自验证；`--experiment E2/E1/E3/E4 --output NEW_DIRECTORY` 中选择一个实验执行其完整编译场景。未注册的实验不会空跑成功。

每例写出 `run.json` 与 `trace.jsonl`：实际源码修订、工具链、配置、ABI/schema、注入/重启、命令提交身份、公开 Goal/Task/Call 变化、独立效果计数、断言及首个失败。它是逐 step 公开观察与外部设施轨迹（含实际批次切断前缀、writer/boot、执行端 deadline/nonce 时间与停止测量），不是内部 journal 每条 transition 的镜像；记录不包含业务正文、密钥或媒体。输出目录须新建，拒绝覆盖已有证据。主失败、关闭失败与产物失败分别报告，任一失败 CLI 非零退出。

| 条款 | 约束 | 反例 | 验证 |
|---|---|---|---|
| C-EXP-001 | 完整 profile 在构建时拒绝未知/重复字段、未实现动作、无效步骤/条款、非法数值及超限；不能将正常 profile 或草案冒充完整实验 | storage_fault 被忽略、零断言脚本报告通过 | `tests/tools/test_generate_experiment_fixtures.py`；完整实验覆盖仍按各后续阶段报告 |
| C-EXP-002 | 故障和重启作用于真实模拟 Host/Kernel；旧生产者先结束借用，持久镜像与外部效果独立保留 | 仅改查询结果变量假装断电，或沿用已释放的 completion | `ExperimentFixtures.compiled_scenario_records_actual_fault_and_assertion_results` 的 HARNESS.S1 场景及 E2 F0～F14；`ExperimentEvidence.oversized_prefix_cut_preserves_arena_and_pending_append` |
| C-EXP-003 | 副作用执行与回执分别控制；独立计数跨 Kernel 重启保留，测试服务不代替 Core 去重 | 每次重启清零计数而掩盖动作重复 | HARNESS.S1.external_effect_survives_cut、HARNESS.S1.completed_receipt_survives_cut |
| C-EXP-004 | 首个失败不被后续匹配检查覆盖，关闭/产物错误仍导致失败；现有目录和未知/空实验拒绝 | 失败后清理返回 OK 把整例改成通过 | `ExperimentEvidence.first_failure_survives_later_success_and_retains_classified_status`；`ExperimentEvidence.unregistered_case_is_rejected_before_creating_output`；`ExperimentEvidence.rejected_recovery_fails_before_empty_world_assertions`；tests/integration/experiment_artifacts_test.py |
| C-EXP-005 | CLI 与 GoogleTest 共用编译脚本及执行器；相同输入两次逻辑证据一致，版本/配置可查 | CLI 只打印预置“通过”，或分别维护两套恢复逻辑 | tests/integration/experiment_artifacts_test.py；`ExperimentFixtures.compiled_scenario_records_actual_fault_and_assertion_results`；`ExperimentEvidence.core_and_provider_clock_stay_equal_with_storage_held` |

E4 为 46 项 Native/Sim 场景；兼容矩阵在 `../e4-compatibility.json`，`ExperimentFixtures.artifacts_replay` 以实际断言逐行验证。旧采样/模型无停止或查询证据时，UNKNOWN 阻断是通过条件，不强求自动完成；已持久停止/结果的路径才接续。用户确认 M1 不做迁移或旧格式 reader，JSON Schema 关键字检查留到 MA-007；相关原用例不计为通过，见 ADR-0079。
