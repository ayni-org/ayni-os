# ADR-0045：统一调用身份、动作事实与派发消费

- **状态**：实施中；本页区分已实现的内部规则与尚未接入的调用链
- **日期**：2026-10-25
- **关联**：ADR-0042～0044；MA-008A；C-CALL-002、008～012；C-COMMIT-005～007

## 背景

资源、Task 运行限制和 Provider 结果交接已分别交付；它们尚不能产生实际调用。MA-008A 需要把稳定调用身份、实际输入、授权、运行次数、动作事实和异步尝试连接起来，不能由任一模块单测推定完整消费已成立。

## 决策

1. `work/call` 拥有 Task/步骤到逻辑调用的关联、输入/结果及尝试；`work/action` 独立拥有稳定 operation、具体 Binding 及副作用知识。后者不持有 Provider 回调、Task 生命周期或资源占用。Kernel 通过现有提交协调器连接各 owner。
2. 动作事实区分 PREPARED、DISPATCHING、IN_PROGRESS、COMPLETED、FAILED、CANCELLED 与 RESULT_UNKNOWN。状态立即应用，是否持久仍由当前 writer 的 `durable_seq` 表达。PREPARED 持久后才准入 DISPATCHING；当前 boot 的屏障持久后且最终复核通过，才可取一次易失 submit claim。
3. 屏障后、交给 Provider 前复核失败，或 submit 明确 NOT_RETAINED，可提交 CANCELLED/effect=NONE。普通取消回执不能形成该结论。单次调用可以直接从 DISPATCHING 收到完成或确定失败，无需伪造进度事件；这是原生命周期表对 unary 失败分支的补全。
4. 结果状态与影响事实分开：失败可以保留部分或未知影响，不能据 FAILED 就推断未执行。持久事实还区分本机未交付、Provider 结果、权威查询结果、权威未受理；普通失败/effect=NONE 不等于权威未受理。所有终态均无原 operation 重发入口。
5. ABANDONED 进入 RESULT_UNKNOWN，普通 complete/fail/dispatch 均不能解除。只有声明满足权威性的查询，且旧尝试已排空或隔离，才能形成对账结论。权威 NOT_ACCEPTED 封闭原 operation 为 FAILED/effect=NONE 并保留该证据类别；之后换 Binding 必须新 operation、新授权与重新准备。重放只重建事实，不恢复易失 submit claim。
6. `work/call` 输入首版采用既有有限 typed 授权字段加有界 UTF-8 文本。`authority/scope` 增加实际请求规范化和规范字节的实际请求检查，复用原有单值、完整性及范围规则；同一份参数进入授权和派发，不另接受声称用途的投影。输入正文 codec 不接收任意 JSON Schema 或厂商 JSON，嵌套模型工具结果随 MA-008C 定义。最大实现字节界限不是产品默认值，也不代表 ESP32 预算。
7. 当前新增 ActionRecord 是内部状态 owner，正常单测验证局部规则；尚无记录类型、journal/replay、实际 Provider、调用计数原子消费或 B4 集成。后续在同一统一调用功能分支继续接入，不能据此标记整个 MA-008A 或派发屏障已交付。

## 后果

同一段状态逻辑用于将来的 live/记录校验，不以散落的布尔字段表示“失败但可能发生”。具体业务输入和摘要仍由逻辑调用保存；ActionRecord 仅保留稳定外部影响身份与知识，避免复制 Task 和授权表。正文保留执行 ADR-0042 的用户决定；删除机制尚待 MA-011。

## 备选方案

把 FAILED 当作安全重试条件会丢失部分/未知影响；把权威未受理仅留在回调局部变量中会使重启后无法判断换 Binding 的依据；直接让 Provider 修改 Task 会破坏 B4。均不采用。

## 验证

内部正常生命周期测试已覆盖两段门控、一次 claim、unary 结果、进度/部分失败、明确未交付、未知结果及权威对账的局部前置条件。有界输入 codec 与实际请求规范化的正常测试也已通过；既有 Scope/Grant 相关回归通过。完整调用集成和对应 PR/CI 尚未完成；没有新增故障、恢复专项或硬件验证。
