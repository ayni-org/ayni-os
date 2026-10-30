# AP-05 长操作与订阅的受管模型控制设计

- **状态**：AP-05 已实现，ABI 0.51 / Core schema 7；正常软件/SDK验证与独立复审见 [#116](https://github.com/RobotSe7en/ayni/pull/116)。本文件保留设计基线盘点，§11记录实际映射与限制。
- **决定**：用户 2026-10-30 选择保留四个固定工具、在 `ayni_call_capability` 中扩展受管操作；引用限原 Task；本包直接升级、拒旧保留目录；最终文字答复结束 Task 并收尾；首版每 Task 同时一个 action，可与模型和订阅并存。见 [ADR-0086](../adr/0086-task-owned-managed-operation-controls.md)。
- **依据**：[基础实施计划](foundation-and-experiment-plan.md)、[能力架构 §8](capability-centered-agentos-plan.md#8-能力调用协议)、[调用与订阅契约](../../contracts/domain/calls_and_bindings.md)、ADR-0045/0063/0084/0085。
- **范围**：以可取消的软件长任务和 BOOL/I64 软件事件源贯通模型、公开策略接口与既有 Core 生命周期；不扩展媒体类型、平台或远程协议。

## 1 要交付的行为

模型能够启动一个软件长任务，取得原 Task 所属的操作引用，继续读取状态或等待变化，再读取真实终态；也能够打开一个软件事件订阅，读取最新有效样本，等待新样本或状态变化，最后关闭。启动受理、Provider 接受、订阅 ACTIVE、业务终态、副作用及资源归还分别表达。

普通 `ayni_call_capability` 和 `ayni_shell` 继续等待原调用终态。新增受管启动是显式选择，不把现有调用默认变成后台工作。模型工具提议仍按顺序处理；长操作保留自己的资源占用，但不占着 Task 的前台调用位置。资源冲突仍排队或明确拒绝，后台启动不构成并行 DAG 或 join 接口。

首版同一 Task 同时只允许一个 TOOL/action，包含已受理排队、执行中、取消中及未解决的 UNKNOWN；MODEL 调用和现有 subscription 可按资源声明并存。第二个 action 在原调用已知终态且不再欠完成借用/资源隔离前返回 BUSY。这个检查归 Core 的统一 Call 准入，普通 invoke 和 `ayni_shell` 也不能绕过。订阅继续保持同 Task/同源只允许一个 desired-open 的原限制。

最终文字答复沿用本轮对话结束语义。仍需工作的模型应显式等待；最终答复触发所属活动操作的取消/关闭收尾，不把操作转给下一轮 Task，也不新增阶段答复或向存活 Task 注入后续用户输入的入口。答复不是“操作已完成”或“已停止”的证据，管理查询继续呈现实际状态。

## 2 已有实现与实际缺口

下表定位基于 main `200222f`（#114）。源码核查仅证明现实现结构，不是 AP-05 执行结果。

| 边界 | 当前实现 | AP-05 要补的内容 |
|---|---|---|
| 模型入口 | [model_messages.hpp](../../include/ayni/model_messages.hpp) 的 `functions` 固定四工具；call 只有 Binding/version/arguments | 为同一固定函数定义封闭的操作分支；通用模型传输保持业务无关 |
| 配置冻结 | [catalogs.cpp](../../core/kernel/config/catalogs.cpp) 的 `Catalogs::close` 明确拒绝 GoalType 工具列表中的 subscription | 按声明的 TOOL 形态允许受管订阅，仍完整验证 source/version、资源、权限和可派发性；不放宽 MODEL 契约 |
| 目录与说明 | [call_directory.cpp](../../core/kernel/service/call/call_directory.cpp) 的 list/inspect 按 Task 当前权限投影；详情含业务契约 | 从同一原契约输出形态、取消能力、可用控制；不能凭工具名称猜测或注册伪控制能力 |
| 工具解释 | [harness_tools.cpp](../../strategies/builtin/harness_tools.cpp) 的 `native_function` 只做发现/检查/一次调用 | 分开调用启动、状态读取和控制；业务 arguments 仍走既有唯一规范化入口 |
| ToolLoop | [tool_loop.cpp](../../strategies/builtin/tool_loop.cpp) 的 `tools` 等 `call_result` 终态，`native_result` 对 pending 返回 BUSY | 为 start 生成受理回执，为 wait 保存待完成的协议往返；一次工具提议不会重建第二条业务 Call |
| 前台引用 | [task_table.h](../../core/work/task/task_table.h) 的 `TaskCall` 只指向阻塞后续 decide 的调用 | 显式受管调用模式；前台引用和后台活动操作的语义分开，不复制 Call 权威事实 |
| 已有后台订阅 | [call_subscription_control.cpp](../../core/kernel/service/call/call_subscription_control.cpp) 在 STARTED 同批切后台并释放前台 | 保留 start ACK、租约和代际；把共同的前后台归属规则用于显式受管 action |
| 完成传播 | [call_propagation.cpp](../../core/work/call_propagation.cpp) 只有后台 subscription 能绕过另一条前台引用 | 后台 action 的进度/终态不得结束或唤醒不相关的前台模型调用；UNKNOWN 仍阻止依赖它的工作 |
| 状态与进度 | [calls.h](../../include/ayni/calls.h) 的 Task 结果视图没有 pending 的完整 phase、等待原因和 progress sequence | 增加 Task-scoped 只读操作投影，共用原 Call、ActionRecord 和资源诊断；进度首版为已有单调 sequence，不新增任意进度正文 |
| 订阅读取 | [call_subscription.cpp](../../core/kernel/service/call/call_subscription.cpp) 限原 Task/decide，返回最新元数据和 usable | 扩展为模型可用的受限读取，绑定来源与有效期；不借 owner 管理查询绕过 Task 权限 |
| 订阅等待 | 同文件 `subscription_wait_basis`/`next_subscription_wakeup`；现 WAIT 在暂存时取最新前沿 | 新模型 wait 必须比较模型上次见到的前沿，避免 read→模型→wait 之间漏掉变化；保留原生订阅消费者的既有语义 |
| 控制提交 | [call_admission.cpp](../../core/kernel/service/call/call_admission.cpp) 的 `stage_subscription_target` 暂存唯一 close/wait 意图 | 受管 cancel/wait 同样经 owner 暂存、完整决定校验和原子提交，不直接改表或回调 Provider |
| 持久与回收 | [call_records.h](../../core/work/call/call_records.h)、[call_restore.cpp](../../core/work/call/call_restore.cpp)、`CallTable::can_retire_task` 保存原调用并保留 pending/UNKNOWN | 保存受管模式、归属和等待前沿；后台 action 也必须参与暂停、撤权、终态、回收及恢复扫描 |
| 对话与结果 | [conversation.cpp](../../strategies/builtin/conversation.cpp)、[conversation_store.cpp](../../runtime/native_node/conversation_store.cpp) 保存控制进度、SQLite 历史与真实结果 | 控制游标写入版本化检查点；操作事实继续归 Core，原始终态继续沿 SQLite 归档，历史文字不成为操作表 |

## 3 模块责任与接口变化

| 属主 | 责任与数据所有权 | 排除的内容 |
|---|---|---|
| `core/work/call` | 原 Call 身份、规范输入、精确 Binding、授权来源、受管模式、前后台归属、进度/取消/终态 | 模型函数名、JSON、另一份操作注册表 |
| `core/work/task` | 单一前台引用、Task 生命周期及显式等待目标/前沿；终态收尾与安全回收的关系 | Provider 指针、复制的结果/租约/资源占用 |
| `core/work/subscription` | 订阅 desired、phase、generation、租期控制事实；保持既有不变量 | 第二套通用 action 状态机 |
| `core/authority`、`resources`、`knowledge` | 原授权覆盖与撤销、资源准入/隔离、样本及来源有效性 | 模型对操作的展示策略 |
| `kernel/service/call` | Task-scoped 投影、暂存、跨属主事务、完成消费和有界唤醒 | 控制策略、软件后台算法或 OS 工作线程 |
| `strategies/builtin` | 解释固定函数、生成协议配对回复、控制算法游标与上下文预算 | 凭 checkpoint 自报操作存在或已完成 |
| Provider、Host、runtime | 异步任务/事件源与线程、公开投递、配置预算；runtime 唯一装配 | worker 查询或修改 Core、控制端口旁路 |

拟在公开 C 接口增加两个窄边界，具体符号在实现时按现有命名落定：

1. **Task 快照只读操作视图。** 接受当前 Task 的稳定引用，投影 Call 形态、精确身份、phase/等待原因、进度前沿、取消诊断、终态/effect、结果耐久水位及可用控制；不发起 Provider 查询。借用仅到 `decide` 返回；需要结果的调用仍共用原结果读取与来源校验。
2. **策略受管请求/控制。** 明确区分普通终态等待、受管 start、cancel、wait 和 subscription close；使用枚举/带 tag 结构，不用布尔参数选择两种算法。控制在回调内立即复制并暂存，和检查点作为唯一决定提交，全部完成校验后才应用。

前后台归属是 Call 的共同不变量。实施时把现有 subscription attachment 移到 Call 的共同事实中，由 action/subscription 共享；订阅特有 desired、generation 和 lease ACK 仍留原属主。Task 不增加后台操作副本列表，复用有界 CallTable 按 task_id 的关联与保留规则。此调整必须同时覆盖原生订阅路径和所有 reader/writer，不能留下两份 attachment。

更换软件 Provider 的检验场景：能力 Schema、Binding 和实现描述按原规则注册，Provider 实现 start/cancel/完成投递；Core、ToolLoop、模型厂商 codec 不增业务分支。若新实现改变取消、查询或副作用契约，按原精确版本/兼容规则处理，不能只替换函数指针。

## 4 固定函数协议与引用

`ayni_list_capabilities`、`ayni_inspect_capability`、`ayni_shell` 的功能保持。`ayni_call_capability` 拟增加 `operation` 分支；省略时仍为当前 `invoke`，本包实现后才发布扩展 Schema。

| operation | 请求选择与前置 | 工具回复与效果 |
|---|---|---|
| `invoke` | 精确 Binding/version/business arguments；保持现有普通调用 | 等待真实终态；不接受 subscription，返回提示使用 start 的明确参数错误 |
| `start` | 同一规范业务请求；支持声明为 TOOL 的 action/subscription，且保持既有可派发性检查 | 首次 Call 接受并持久后回复原引用与真实当前阶段；QUEUED/STARTING 不声称 Provider 已开始或订阅 ACTIVE |
| `read` | 原 Task 所属操作引用 | 返回当前受限状态、前沿、可用控制及已知结果；没有网络 I/O，不把 NOT_FOUND 推为未执行 |
| `wait` | 引用、模型上次实际见到的 `after` 前沿及显式有限等待时长 | 已变化则立即回复真实最新视图；否则等待 owner 唤醒或超时，再完成同一个 tool_call_id 的回复 |
| `cancel` | action 引用及原 operation 身份；Provider 契约允许取消 | 持久记录取消请求；回复“请求已受理”和实际状态，不声称停止；不产生新业务调用 |
| `close` | subscription 引用及预期 opening 身份 | desired 置 closed 并关 gate、停止续租；回复实际 STOPPING/终态；等待停止可继续使用 wait |

控制字段置于函数封套，业务字段只在 `arguments` 内。每个分支有明确允许/必填字段；拒绝未知字段、重复键、互斥字段并存、跨分支字段和不支持的形态。共用 lossless arguments 解析，不能用业务 JSON 额外携带权限或控制句柄。Shell 要在后台执行时可在发现/检查后对其精确 Binding 使用 start；`ayni_shell` 本身保持现行为。

操作引用是版本化、有界的值，绑定原 Task 和 logical Call；没有指针、槽索引或 Provider 句柄。引用不授予权限，owner 每次检查当前 decide 的 Task/owner 与原记录。终态/已回收/另一 Task 的引用有分类结果，不回退到“按相似 Binding 再启动”。同一 Agent 的下一轮 Task 也不能借历史引用控制上一轮。

64 位 Call、operation、boot、generation 和 sequence 在 JSON 中用精确字符串表示；32 位预算和版本仍验证正数与上限。状态返回 `after` 前沿；cancel/close 另核对原 operation/opening，迟到旧代控制返回 STALE，不作用于新代。read 可以报告原逻辑订阅的新代际及新前沿，让仍存活的原 Task 显式选择后续控制。

协议 `tool_call_id` 只负责消息配对。每条提议由原 Model Call 加 ordinal 生成稳定控制 step；重入/正常继续读取同一已提交处理结果，不重新 start，不用模型自报 ID 替代 Core 身份。start 的“受理回执已处理”和业务终态分开；原业务 Call 仍是执行与结果的唯一证明。

| 分类结果 | 含义与状态影响 |
|---|---|
| INVALID_ARGUMENT | 操作封套、形态或前沿非法；没有新业务 Call 或控制事实 |
| NOT_FOUND | 引用不存在或不属于当前 Task；不披露另一 Task 的存在/正文 |
| STALE | 原 opening/operation 已变化；旧控制不作用于新代 |
| BUSY / CAPACITY_EXCEEDED | 暂时背压或硬容量不足；被拒决定不留下部分状态，已接受操作继续可查 |
| UNSUPPORTED | 声明不支持该控制，例如不可取消 action；不伪装成取消受理 |
| EXPIRED | 来源正文失效或本次有限 wait 到期；分别标明原因，不改变原执行 outcome/effect |
| RESULT_UNKNOWN | 原执行知识仍未知；保留引用与诊断，不自动重做或返回普通成功结果 |

## 5 启动、完成与 Task 生命周期

受管 start 的 Call 接受、规范输入/原契约/完整 Grant 绑定、归属以及 ToolLoop 控制检查点在同一 transition。先持久再向后续模型披露受理引用；实际派发仍走原资源准入和持久派发屏障。Provider 的 submit 接受不被解释成启动或完成。

action 在接受为受管模式后允许原 Task 继续决定；这只解除该 Call 对 Task 前台的阻塞，资源和完成借用不释放。subscription 仍等可靠 STARTED 才进入 ACTIVE、开放样本 gate；模型可以先收到真实 STARTING 的受理视图，再显式等待 ACTIVE 或新样本。

后台进度/终态沿原邮箱和 CallTable 应用。普通前台模式沿用现 `end_task_call`；受管模式更新原 Call，不得 `finish_call` 另一条前台模型调用，也不得把 action 完成当成 Task 自动完成。UNKNOWN 保留原身份和 effect，停止依赖该结果的新工作；模型不能以换 Binding、start 或新 operation 绕过它。owner 原有显式对账能力保持，模型 read 不获得管理会话权限。

Task 暂停、取消、授权/来源失效、Goal/Agent 结束和 Kernel shutdown 都必须扫描名下受管操作。取消/关闭请求、Provider 取消回执、终态、投递借用归还及资源释放分别推进。最终答复结束 Task 时触发同一收尾机制；即使文字可以发布，原操作尚未终态的诊断和 Task 保留关系仍存在，不伪造停止确认。

已完成/已失败/已取消 Call 的事实和结果保持可查。终态 Task 的 pending、UNKNOWN、租期隔离或仍借出的投递阻止安全回收；不得为容纳下一轮对话淘汰它。原生消费者不选择受管模式时行为保持。

## 6 等待、租约与背压

wait 选择一个原 Task 的操作和前沿；首版没有 wait-any、多目标 join 或逐帧积压。action 前沿至少含原 operation、已应用 progress_sequence、取消状态和终态；subscription 前沿含 opening、phase、最新样本 sequence/gap，排除同状态 renew ACK。具体值由 Core 输出，模型原样带回，不允许模型任意未来前沿让等待永远不满足。

owner 在应用 wait 的同一 transition 内比较 `after`：

1. 前沿已变化或已终态时，不进入等待，返回真实当前状态；不会把“开始等之前已到的样本”吃掉。
2. 前沿未变时，登记唯一等待目标、前沿和有限时长，并同时保存尚欠 tool 回复的检查点；复用 WAITING，不增加并行等待布尔量。
3. 新前沿由 owner 消费后唤醒一次；progress/样本可以合并到最新状态，取消和终态仍走可靠独立 lane。
4. 有限等待到期返回超时及实际当前状态，不自动取消原操作或返回成功。时长受部署正数硬上限和原 Task/调用期限约束，不采用无限等待默认值；精确预算在实现的配置量测中明确。

原生 WAIT_SUBSCRIPTION 的“暂存时取当前前沿”保持；新通用 wait 明确接收 expected frontier，两者复用同一比较和唤醒属主。新 Task 等待值显式带 action/subscription tag；零值只在已命名的 none 分支出现。更新 Task revision 预留规则，等待、唤醒和最终取消都有空间，管理操作不能花掉保留余量。

订阅保留默认 30 秒、半租期系统续租及原完整权限/运行限制/资源复核。模型等待、响应耗时或消息压缩不参与续租。关闭/到期先关 gate 并失效缓存，旧 opening 和迟到 ACK 不复活它。UNKNOWN 仍按最长已请求租期隔离资源；受理取消不归还生产者借用。

start 前检查同 Task 单 action 约束并预留 Call、控制检查点、结果/终态邮箱和回复容量；不足明确拒绝，失败决定不留下半个操作。队列、CallTable、模型上下文和 SQLite 各自的容量/失败保持独立。高频观测 FIFO 满拒绝并保留可靠 gap；模型只取最新有效值，不以逐帧调用模型维持租约。

每个新模型控制提议仍计入 Task 的有界步骤预算，后续模型与摘要继续消耗原模型次数；相同工具提议重入不重复计数。单个 wait 的唤醒不按每条样本追加步骤，续租不消耗模型次数。不能通过 read/wait 循环重置 Task 限制或把有限步骤变成无限日志。

## 7 权限、样本与上下文

start 复用 AP-02 的唯一业务规范化、完整授权投影和 AP-03 的按需 Grant 绑定；接受与派发、续租分别按原规则复核。引用不是授权替代物，read 不扩大目录披露或读取范围。撤销已使用授权仍暂停所属 Task，不自动选择另一 Grant 继续。

cancel/close 属于原 Task 的停止控制，不要求新增执行授权；只针对原 operation/opening，仍通过受限策略入口暂存。read/wait 不能把 owner 的 CALL_QUERY 管理命令包装成模型工具，也不能在撤权 Task 已暂停时偷偷继续模型交互。

订阅 read 的首版正文只有现有 BOOL/I64、单位、来源分类、质量、采集时间/有效期、完整 ObservationRef、usable 与 gap。只有有效样本作为分析输入；不可用样本返回分类状态和必要诊断，不伪造值或读取媒体。任意结构化事件/媒体不由本包暗增。

当前订阅读取不会自动建立完整的“样本→工具回复→模型调用→历史摘要”来源链，这是模型接入必须补齐的消费者边界。实施须复用 knowledge/context 的依赖所有权，保存原 ObservationRef 与所需读取/外发授权；在实际模型接受、派发和结果消费处复核。关闭、代际变化、gap、过期或原来源失效后，旧工具正文及其派生摘要不得再次进入新上下文。原新鲜度规则保持；不能把曾读到过或 SQLite 留有原文解释成当前有效。

受理回执和状态响应中的原 Call 身份/outcome/effect 与可能失效的样本正文分开。上下文重建从真实账本恢复事实，不重做操作；摘要保持未确认来源，失败保留原文并暂停。原始终态结果沿现有 SQLite 大结果/分块摘要路径处理，受管接口不另建结果存储。

## 8 线程、持久与版本

公开查询、decide、submit/cancel/renew 继续由 owner 线程执行且有界。Provider worker 只使用已复制的参数和原 attempt/opening，通过公开非 ISR post 入口投递；不能回调 owner API。start 的后台含义不改变 borrowed input、terminal post 返回和 owner ACK 的责任。Kernel 销毁仍以 drain/quiesce/join 完成为前提。

Call 保存受管模式及唯一前后台归属，Task 保存 tagged wait/前沿和相对等待时长/正常恢复规则，ToolLoop checkpoint 只保存协议处理进度和对这些事实的引用。journal/snapshot 共用原属主 codec 与 validator；跨记录验证检查 Task—Call 归属、mode/attachment、等待目标和同批配对。不得持久化指针、mutex、活线程或跨 boot 可用的 monotonic 截止。

action 正常重开按原派发事实恢复：已完成步骤不重复，确定未派发重新准入，可能派发保留 UNKNOWN 并等待显式 owner 对账。后台模式不变成恢复重试许可。subscription 仍只对已知停止且 preserved intent 以新代际重开；FAILED/EXPIRED/UNKNOWN 不自动重开。恢复的旧样本和旧等待绝对时刻不可当作有效；保存的有限等待按明确的当前 boot 规则重建，不能延长原 Goal/Task 期限或无限续期。

本包兼容决定已由用户确认：直接升级并重编译全仓消费者，新写入前拒绝旧目录并保留，无旧 reader/迁移器；回退使用原二进制与原目录。按当前基线，公共布局与持久状态变化预计需要下一 ABI minor/Core schema；具体号码在实施前核对。本文不修改 0.50/6 的头文件或当前存储契约。

Core schema、能力语义版本、固定工具协议、消息分帧、策略 checkpoint、SQLite 应用版本分别判定。机器 Schema 不变时不为实现方式随意改能力语义；工具定义只在新版本启动时确定，不在线切换。checkpoint 新字段必须升级 reader/writer 声明；SQLite 仅在实际布局/解释规则变化时升级，不能只改 Core schema 就假定外层兼容。

## 9 消费者与迁移顺序

| 消费者 | 实施内容与复核边界 |
|---|---|
| `include/ayni/ayni.h`、`calls.h`、`ayni.hpp` | 受管模式、Task 只读投影和控制暂存；C99/无异常 C++、struct_size、借用、分类失败和 Since 注释 |
| catalog、GoalType 工具策略、directory | 接受合规订阅、完整说明形态/控制能力；当前权限和元数据来源规则保持，不新增控制 Provider |
| CallTable、TaskTable、call_propagation | 一处维护共同 attachment 与 tagged wait；后台完成不影响前台；安全回收与 UNKNOWN 保留 |
| CallRuntime、完成邮箱与 observation ingress | 受管接受/取消/等待原子提交、可靠完成、有界唤醒与来源失效；复用原 transport/租约机制 |
| authority/resources/knowledge/context | 原 Grant 精确绑定、停止无需新执行权、资源隔离、样本进入模型后的完整来源依赖；必要接口归各自属主 |
| record/store/replay/snapshot、arena/预算 | mode/attachment/wait 字段、跨表配对、最大记录/批次/快照和新 schema；结构校验不靠恢复时重跑业务 |
| 固定工具、ToolLoop、检查点与消息历史 | 操作封套、精确字符串引用、工具消息配对、wait 检查点、来源擦除、真实状态和 token 预算 |
| OpenAI-compatible Provider 与 Mock 模型 | 使用同一固定函数 Schema，通用协议解码；不增 action/subscription 业务 JSON 分支 |
| 软件 Provider、Native/Sim runtime 与 SDK 示例 | 一个可取消作业、一个无媒体的软件事件流；共享应用/配置边界，不复制 Kernel 编排或跨线程调用 Core |
| 助理、规则、Shell、现有订阅/远程 typed 夹具 | 仅迁移受影响公开布局和关联规则；保留普通调用行为，不把本包变成完整 MA-038 或新平台实验 |

按一个完整 AP-05 功能组织实现 PR，内部提交依次为：共同状态/记录与预算、公开接口及受管生命周期、固定工具与上下文、软件示例和正常测试、文档。若来源链或公开接口分析证明需要独立交付边界，先明确依赖和理由，不按文件数量机械拆 PR。

## 10 验证与完成条件

| 场景 | 可判定的预期 |
|---|---|
| 长任务 start→read→wait→terminal | 原 Task/Call/Binding/Grant 不变；受理与实际完成分开；后台运行期间本地任务及模型槽可继续推进 |
| 一个 action、subscription 与前台模型调用 | 按实际资源准入；后台完成不结束另一条前台调用；第二 action（含 invoke/Shell）明确 BUSY，不留下新 Call |
| subscription start→ACTIVE→read→wait→close | STARTED 前无可用样本；模型只读本 Task 最新有效值；续租无需模型参与，close 不伪造 STOPPED |
| read 与 wait 之间先有变化 | 带回旧前沿立即得到新视图；真正等待后只唤醒一次；同状态 renew 不触发模型 |
| 正常取消/关闭与最终文字答复 | 请求、控制回执、终态及资源归还可区分；结束 Task 不遗留可继续控制的下一轮引用 |
| 两 Task 同源及引用边界 | 样本/资源和关闭隔离；另一 Task、旧 opening、畸形或未来前沿不获得控制或正文 |
| 公开容量与来源边界 | Call/控制/回复/上下文满时明确拒绝；样本失效、关闭或撤权后旧正文不进入新模型请求；原 outcome/effect 仍真实 |
| 原消费者正常回归 | 普通 invoke/Shell、原生 WAIT_SUBSCRIPTION、现有订阅、SQLite 归档/压缩/删除及现有 E1～E4 继续通过适用检查 |

正常软件场景经公开 C API 和模型协议夹具验证；测试注释关联 C-CALL-003/005/008～012、C-SUB-001～005、C-POLICY-004/005/007/008 及本 ADR。可以定序控制正常样本和完成交付，不另开崩溃切断、损坏镜像、恢复注入或变异专项。当前既有测试仍全部保留；新增持久格式的故障/恢复证据保持未验证，基础验收不能用正常用例替代。

既有回归入口包括 [kernel_call_test.cpp](../../tests/unit/kernel_call_test.cpp)、[kernel_subscription_test.cpp](../../tests/unit/kernel_subscription_test.cpp)、[provider_mailbox_test.cpp](../../tests/unit/provider_mailbox_test.cpp)、[strategies_test.cpp](../../tests/unit/strategies_test.cpp) 和 [native_assistant_test.cpp](../../tests/unit/native_assistant_test.cpp)；它们当前的正常调用/订阅/消息结果不能替代新增受管接口的场景证据。

实施前检查主机资源，再显式选择构建/测试并行度。按影响运行本地正常与输入边界测试，最终适用 CI、设计自查和 Core 独立审查；公共布局/容量改变时量测 arena 默认/最小/双 Agent 配置，记录实际结果。新增调用关联与有界扫描的开销先量测，不据 Native 数字宣称 MCU 或硬实时支持。

本轮文档交付检查为 diff、契约追踪与链接/计划一致性；不将源码盘点列为测试通过。AP-05 实现完成后才更新支持状态，随后 AP-06 和 AP-04 多实例证据收尾，再进入基础验收。


## 11 实现映射与当前限制

公开实现为 `operations.h` 的复制引用/前沿/读视图和 `policy_output.control_operation`；`calls.h` 的 mode 默认 FOREGROUND，MANAGED 只适用 TOOL action/subscription。Task 诊断用 wait_logical_call_id/form/generation/sequence/phase/timeout_ms，原订阅专用字段名同步升级。SDK 示例见 `examples/managed_operations.cpp`，实际模型往返见 `ManagedOperations` 正常用例；无硬件、媒体、远程服务或付费模型。

引用编码为 `o1:<Task>:<Call>:<operation>`，前沿为 `f1:<boot>:<sequence>:<gaps>:<phase>:<cancel_requested>:<cancel_result>:<outcome>`，所有数值保留精确十进制，无前导零。单次 wait 当前实施上限30000ms，是暂用默认值，未把待答的时长选择记成新增用户确认。到期返回独立 wait_status=EXPIRED；操作仍继续。对不可用样本只返回 sample=null，执行身份/outcome/effect 保留。

CallState 独占共同 attachment；Task 独占 tagged wait 和独立 call_unknown overlay。Task 不复制后台列表，额外 revision/queued 证明通过协调层读取原 Call。Core schema7 的 CALL_STATE 固定424 B，TASK_STATE132 B；deadline 不编码，有限wait在新boot中断后回读。策略格式4、Native内置策略版本3、SQLite user_version2 分别升级。Native目录锁内先检查已有 log/snapshot 的原始 schema，再允许外层数据库创建；Core仍做完整CRC/语义检查。直接升级、不迁移，回退保留原二进制与原目录。

样本来源由原 Call/knowledge 的精确引用维护，Call ModelContext 与持久对话记录均保留最多8项来源；模型接受、派发和结果消费重新检查。失效时移除整个旧协议组及派生正文，从原账本重建当前操作引用/事实，并为实际新样本重新捕获来源；已提交控制不重放。新模型请求不携带孤立旧tool消息。新增扫描均有配置容量界，软件Provider每槽每advance至多256次加法或一次投递；不据此声明MCU或硬实时能力。

新格式故障/恢复专项、变异、真实模型、硬件和平台实验仍未验证；既有回归继续保留。AP-06/AP-04 与基础验收不由本实现自动完成。

资源准入沿用同优先级 Agent 实例内FIFO（C-RES-004），受管模式不暗改优先级或插队：若更早的 action 尚未取得资源，后续同Agent的MODEL可暂时排队，期间可从owner管理入口取消Task；action已准入运行后模型/订阅可按各自资源并存。原生策略的本地read/wait不需要MODEL槽，资源超时后仍能显式恢复同一调用。
