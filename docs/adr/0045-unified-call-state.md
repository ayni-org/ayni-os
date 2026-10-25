# ADR-0045：统一调用身份、动作事实与派发消费

- **状态**：已接受；本页区分内部契约与尚未接入的调用链
- **日期**：2026-10-25
- **关联**：ADR-0042～0044；MA-008A；C-CALL-002、008～012；C-COMMIT-005～007

## 背景

资源、Task 运行限制和 Provider 结果交接已分别交付；它们尚不能产生实际调用。MA-008A 需要把稳定调用身份、实际输入、授权、运行次数、动作事实和异步尝试连接起来，不能由任一模块单测推定完整消费已成立。

## 决策

1. `work/call` 拥有 Task/步骤到逻辑调用的关联、输入/结果及尝试；`work/action` 独立拥有稳定 operation、具体 Binding 及副作用知识。后者不持有 Provider 回调、Task 生命周期或资源占用。Kernel 通过现有提交协调器连接各 owner。
2. 动作事实区分 PREPARED、DISPATCHING、IN_PROGRESS、COMPLETED、FAILED、CANCELLED 与 RESULT_UNKNOWN。状态立即应用，是否持久仍由当前 writer 的 `durable_seq` 表达。PREPARED 持久后才准入 DISPATCHING；当前 boot 的屏障持久后且最终复核通过，才可取一次易失 submit claim。ActionRecord 固定原位保存、禁止 copy/move，prepare 不能覆盖已有对象；全局 operation 唯一性仍由表及分配器保证。
3. 屏障后、交给 Provider 前复核失败，或 submit 明确 NOT_RETAINED，可提交 CANCELLED/effect=NONE。普通取消回执不能形成该结论。单次调用可以直接从 DISPATCHING 收到完成或确定失败，无需伪造进度事件；这是原生命周期表对 unary 失败分支的补全。
4. 结果状态与影响事实分开：失败可以保留部分或未知影响，不能据 FAILED 就推断未执行。持久事实还区分本机未交付、Provider 结果、权威查询结果、权威未受理；普通失败/effect=NONE 不等于权威未受理。所有终态均无原 operation 重发入口。
5. ABANDONED 进入 RESULT_UNKNOWN，普通 complete/fail/dispatch 均不能解除。只有声明满足权威性的查询，且旧尝试已排空或隔离，才能形成对账结论。权威 NOT_ACCEPTED 封闭原 operation 为 FAILED/effect=NONE 并保留该证据类别；之后换 Binding 必须新 operation、新授权与重新准备。重放只重建事实，不恢复易失 submit claim。
6. `work/call` 输入首版采用既有有限 typed 授权字段加有界 UTF-8 文本。`authority/scope` 增加实际请求规范化和规范字节的实际请求检查，复用原有单值、完整性及范围规则；同一份参数进入授权和派发，不另接受声称用途的投影。输入正文 codec 不接收任意 JSON Schema 或厂商 JSON，嵌套模型工具结果随 MA-008C 定义。最大实现字节界限不是产品默认值，也不代表 ESP32 预算。
7. 当前新增 ActionRecord 是内部状态 owner，正常单测验证局部规则；20/v1 动作事实 codec 已实现但尚未接入 journal/replay、实际 Provider、调用计数原子消费或 B4 集成。动作状态、输入/记录 codec 与静态调用契约作为一组可独立验证的内部调用契约交付；下一完整功能再接公开 Provider/Binding 与消费链，不能据此标记整个 MA-008A 或持久派发已交付。此边界与外部异步 I/O 生命周期分开审查。

8. `catalog/capability` 拥有调用形态、模型类别、副作用、幂等/查询/取消、时限、文本格式/上限及资源组的静态契约。资源组规范排序且重复拒绝；匹配逐项精确比较，类别为 MODEL 的调用不会因使用普通请求入口而绕过计数。目录接受可识别的 PHYSICAL/subscription 元数据不代表可执行；消费端在对应机制交付前须明确拒绝。输入使用完整有限参数加文本，结果 format 1 为有界 UTF-8 文本；工具结果扩展随后独立版本化。

## 后果

同一段状态逻辑用于将来的 live/记录校验，不以散落的布尔字段表示“失败但可能发生”。具体业务输入和摘要仍由逻辑调用保存；ActionRecord 仅保留稳定外部影响身份与知识，避免复制 Task 和授权表。正文保留执行 ADR-0042 的用户决定；删除机制尚待 MA-011。

## 备选方案

把 FAILED 当作安全重试条件会丢失部分/未知影响；把权威未受理仅留在回调局部变量中会使重启后无法判断换 Binding 的依据；直接让 Provider 修改 Task 会破坏 B4。均不采用。

## 验证

内部正常生命周期测试已覆盖两段门控、一次 claim、unary 结果、进度/部分失败、明确未交付、未知结果及权威对账的局部前置条件。有界输入 codec 与实际请求规范化的正常测试也已通过；既有 Scope/Grant 相关回归通过。独立审查发现并关闭了复制/覆盖可能复活 submit claim 的问题，其余内部规则未发现功能阻断。动作事实 codec 正常往返与静态契约注册/匹配测试通过。完整调用集成尚未完成，适用检查与 CI 证据集中在 [PR #63](https://github.com/RobotSe7en/ayni/pull/63)；没有新增故障、恢复专项或硬件验证。

## 补充决定：公开调用与主动对账（2026-10-25，实施中）

- **所有者可在撤销执行 Grant 后继续对账（产品语义，用户选定推荐项）。** 管理端只能以原逻辑调用身份查询原 Provider 的原 operation，必须有受信任所有者的 MANAGEMENT 会话、非零命令身份和查询 revision。对账不重新授权执行、不更换 Provider、不重试原 operation，也不增加 Task 模型执行次数。
- 原执行 attempt 与查询 attempt 分开保存；一次管理命令只接受一个查询代际。相同命令的重交返回原受理结果，不再次调用查询端口。查询先保存独立派发屏障；已可能发出的查询不会在重载后自动重发。
- 沿用原决策 5 与 C-LIFE-010：只有原 Provider 声明 AUTHORITATIVE 的查询结果才能解除 UNKNOWN。BEST_EFFORT 的 FOUND 和未找到均保留 UNKNOWN；NOT_ACCEPTED 只有在权威服务保证原 operation 被封闭且迟到请求不能执行时，才能记录权威未执行结论。
- 查询覆盖未知结果及已报告 FAILED/CANCELLED 但 effect=UNKNOWN 的未解影响；否则合法终态会永久占槽且无法由所有者对账。查询失败或未知只更新查询诊断，保留原执行事实。查询成功与仍等待原调用的 Task 进度同批提交；已取消 Task 不复活。查询本身可取消，但取消请求不等于借用已归还，仍等待可靠终态回执。
- **资源等待超时后显式恢复原 Task（产品语义，用户选定推荐项）。** 30 秒等待超限只持久暂停，保留未派发调用身份、输入、进度及已接受的模型次数。所有者 MANAGEMENT 会话使用非零命令身份与 Task revision 提交 TASK_RESUME_WAIT；重排队不重复计数。该操作只解除资源等待原因；用户暂停、授权暂停与兼容 hold 独立保留。普通 Goal/实例恢复不能清除此等待暂停，取消仍可直接结束未派发调用。Task 状态使用 REQUIRED type6/v12，与 exact type9/v8 命令同批配对，旧 reader 明确拒绝。
- **取消未知调用的 Task（按推荐项实现，改选再改）。** 当原 Provider 已归还本机借用，用户可结束 Task；未知调用及动作事实继续保留，不能自动重试或随 Task 槽回收。该取消语义尚未获得单独明确选择。

以上是当前分支实施决定；公开链与对账的最终验证、独立审查和 CI 尚未完成，不以本文新增内容宣布 MA-008A 或 M1 完成。
