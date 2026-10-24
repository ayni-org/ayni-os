# 运行限制

- **关联**：ADR-0042；计划 §2.3、§5.1、§14.1；MA-018A、MA-008
- **属主**：`work/task` 保存 Task 限制与消耗；`resources` 管理并发槽；Provider 管理单次 I/O 边界；可信应用配置后台频率。
- **范围**：不做累计费用钱包、预算层级、充值、预留或费用结算。旧 C-BUDGET 条款保留编号并废止。存储空间及 step 工作项/时间预算仍是独立机制。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-RUN-001 | 每 Task 有有限模型调用次数，单次请求有超时与输入/输出大小限制；默认模型并发 1、每 Task 调用 8 次、超时 30 秒；配置必须在启动硬容量内 | 将余额检查取消后变成无限模型循环 | M1 部分实现（Task 上限）：`TaskLimitApi.startup_ceiling_can_reduce_initial_limit_without_affecting_steps`；Provider 超时/字节边界待 MA-008 |
| C-RUN-002 | Task 调用计数与稳定调用接受事实同批持久，普通恢复或普通 resume 不重置；重复结果不重复计数 | 暂停再恢复后重新得到 8 次调用 | MA-008A；待实现，恢复专项暂停 |
| C-RUN-003 | 达到限制时保留进度并暂停；只有所有者认证管理命令携带 command_id 与期望版本可提高软上限，解除该项暂停，不解除其他暂停 | 模型返回更高限额，或加额同时解除授权暂停 | M1 状态/管理机制已实现：`TaskRuntimeLimits.limit_hold_keeps_progress_and_other_tasks_ready`、`TaskRuntimeLimits.ordinary_pause_resume_does_not_reset_count_or_limit_hold`、`TaskRuntimeLimits.raising_preserves_policy_and_compatibility_holds`、`TaskLimitApi.owner_raise_is_exact_idempotent_and_preserves_policy_hold`、`TaskLimitApi.validates_cas_hard_ceiling_and_live_state_before_changing_anything`；生产调用消费待 MA-008A |
| C-RUN-004 | 一个 Task 达限制不阻止其他 Task 的合法工作；资源与计数全部检查通过才同批准入 | A 达上限后 B 也无法调用，或资源已占用但计数检查失败 | MA-018A、E1 正常子场景；待集成 |
| C-RUN-005 | 模型输出不能修改端点、密钥、硬容量或后台频率；不得自动换服务或参数重试掩盖未知结果 | 不支持参数的请求已经外发后换另一参数自动重发 | MA-008C；待实现 |
| C-RUN-006 | usage/token/bytes 是可选诊断事实，不形成累计费用账本；取消请求不证明未执行、未外发或未计费 | 将本地连接关闭报告为远端未接受 | MA-008；待实现 |
| C-RUN-007 | 前台等待超限暂停并说明原因，提醒可声明无界等待，后台可跳过但须计数；停止链绕过普通资源排队 | 后台永久等待且无状态可查 | M1 部分实现（资源等待策略，尚不改 Task）：`ResourceAdmissionTest.expired_waits_report_action_before_acquisition_and_reminders_can_wait`；Task 消费与计数待 MA-008/MA-019 |
| C-RUN-008 | 后台工作频率来自可信配置，有明确最短间隔；模型不能通过连续创建子任务绕过该上限 | 同一观察目标每次换 Task ID 后立即再次外发 | MA-019；待实现 |

ABI 0.30：`ayni_config_v1.task_model_call_hard_limit` 为 1..64 次，0/省略取 64；新 Task 软上限取 min(8, 硬上限)，与并发槽/步骤容量无关。`ayni_get_task` 追加 used/limit/hard_limit/limit_reached。`TASK_RAISE_MODEL_LIMIT`（13）携带 Task ID、新总上限及 envelope CAS/command_id；认证管理入口（含重试）只允许严格提高活 Task，原进度、计数和其他暂停不变。该命令仍遵循 APPLIED/DURABLE 与有界命令窗口，改变上限本身不执行 Provider。

在最后一次允许的调用受理时计数达到上限；其结果仍可完成 Task。下一次超额请求才置独立限制暂停，不扣额、不分配调用身份；实际调用原子性由 MA-008A 接入。状态 codec 正常往返见 `TaskRuntimeLimits.runtime_codec_preserves_count_limit_and_hold_and_keeps_legacy_defaults`，命令 codec 见 `TaskLimitCommand.codec_binds_task_revision_and_total_limit_to_exact_result`。恢复格式代码已接入，但本轮没有新增重启/断电专项验证。
