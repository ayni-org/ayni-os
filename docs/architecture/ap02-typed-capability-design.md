# AP-02：通用能力参数、结果与授权投影设计

- **状态**：基础盘点与实施设计；尚未修改生产代码或公共接口。类型、数值、列表授权和兼容范围已由用户 2026-10-29 确认，见 [ADR-0084](../adr/0084-typed-capability-values-and-authority-projection.md)。
- **依据**：[基础能力实施与实验计划](foundation-and-experiment-plan.md)、[能力架构](capability-centered-agentos-plan.md)、[调用契约](../../contracts/domain/calls_and_bindings.md)。
- **基线**：代码为 #109、ABI 0.47 / schema 3；定位与路线文档为 [#110](https://github.com/RobotSe7en/ayni/pull/110)。本文描述拟实现行为，不将只读核查记为执行证据。
- **本轮交付**：交付路线文档 PR，完成已有基础与调用消费者盘点，形成本设计及决策记录。本包生产实现随后按本设计另行开展；不进入平台、硬件、付费模型或新测试专项。

## 1 参数与结果属于哪里

它们是每个已注册能力的调用接口：Agent/策略提交输入，Ayni 校验并授权，Provider 执行，再向原 Task 返回结果。例如软件批量计算的输入可以含 options 对象、items 列表、精确单价及近似测量值，结果可以含逐项计算结果和汇总。字段语义归能力定义；Core 提供类型、权限、生命周期与持久机制。

本包首版支持有界对象、列表、布尔、整数、枚举/标识符、UTF-8 文本、有限浮点和精确十进制。支持这些类型不等于接受任意 JSON Schema，也不要求宿主提供文件、相机、电机或其他特定能力。

## 2 基础基线与实际缺口

| 基础范围 | 已有事实与证据入口 | AP-02 的处理 |
|---|---|---|
| 生命周期、工作与调度 | M1 的 Kernel、Agent/Goal/Task、触发、检查点与取消；[Core Alpha 报告](../../reports/core/m1-core-alpha.md) | 保留属主和状态机；仅迁移调用接口消费者，不重写调度 |
| 授权、资源与调用 | 有限 Grant/Scope、审批/撤销、运行限制、资源准入和三类调用；[契约追踪](../../contracts/traceability.md) | 类型系统与授权域分开；实际值产生完整授权投影，不扩大 Grant |
| 持久、恢复与幂等 | 当前 schema 3、原始 Call 事实、派发屏障、UNKNOWN 处理及 Native/Sim E1～E4 | 新格式保存解释旧调用所需的原始业务契约；不通过新目录重释输入 |
| 记忆与上下文 | 私有空间、来源、删除 lineage、结果依赖与耐久门控 | 新节点表/编码副本纳入擦除和依赖失效，不改成新的记忆系统 |
| SDK 与应用 | C99/C++17 入口、ToolLoop、助理及示例 | 增加通用值读写便利接口；助理仍为应用样例 |

当前 typed 工具提议并不是通用业务类型系统。下表来自实际调用链的静态核查，不是新类型的执行证据；#110 已运行的既有 CI 也不代表尚未实现的 AP-02 通过：

| 边界 | 现实现证据 | 缺口与迁移 |
|---|---|---|
| 请求、提议、执行输入 | [calls.h:92](../../include/ayni/calls.h#L92)、[calls.h:107](../../include/ayni/calls.h#L107)、[calls.h:300](../../include/ayni/calls.h#L300) 均为 authority scope 加 text | 用规范业务值取代公开调用者另交的 authority 值及不透明正文；Binding/step 身份保留 |
| 结果、查询与工具视图 | [calls.h:118](../../include/ayni/calls.h#L118)、[calls.h:247](../../include/ayni/calls.h#L247)、[calls.h:284](../../include/ayni/calls.h#L284)；[call_api.cpp:81](../../core/kernel/api/call_api.cpp#L81) | 增加业务 Schema/结构化结果视图，查询不能再把所有正文称为 UTF-8 text |
| 注册和 exact 匹配 | [call_contract.cpp:62](../../core/catalog/capability/call_contract.cpp#L62) 只收 FIELDS_TEXT；[call_contract.cpp:101](../../core/catalog/capability/call_contract.cpp#L101) 比较契约 | Schema、默认值、精度、投影及限制一并规范化、复制、精确比较 |
| 规范输入 | [call_input.cpp:28](../../core/work/call/call_input.cpp#L28) 只规范化授权字段后追加 text | 新增可复用的业务值规范化，默认值、树形、精度和授权路径一次定义 |
| 准入及派发 | [call_admission.cpp:115](../../core/kernel/service/call/call_admission.cpp#L115)、[call_admission.cpp:208](../../core/kernel/service/call/call_admission.cpp#L208)、[call_dispatch.cpp:152](../../core/kernel/service/call/call_dispatch.cpp#L152) | 同一份规范值用于接受与屏障后复核；列表检查固定同一 requirement/Grant |
| 完成入口与可靠邮箱 | [provider_completion_api.cpp:52](../../core/kernel/api/provider_completion_api.cpp#L52)、[provider_mailbox.cpp:15](../../core/kernel/ports/provider_mailbox.cpp#L15) | 结构化结果/提议校验后复制，保留 attempt、预留容量、独立控制槽和 ACK 规则 |
| 模型 codec 与复制 | [protocol.cpp:100](../../providers/openai_compat/protocol.cpp#L100)、[protocol.cpp:298](../../providers/openai_compat/protocol.cpp#L298)、[provider.cpp:314](../../providers/openai_compat/provider.cpp#L314) | 从固定 field_ID+text 改为通用树；Schema、值和说明在 submit 内复制，worker 不借用 Core |
| 说明端口 | [tool_usage.h:35](../../include/ayni/tool_usage.h#L35) | 参数说明改按业务字段身份/路径配对，不能继续把业务字段数等同于至多 8 个授权维度 |
| 策略/应用 | [support.cpp:124](../../strategies/builtin/support.cpp#L124)、[tool_loop.cpp:165](../../strategies/builtin/tool_loop.cpp#L165)、[tool_loop.cpp:248](../../strategies/builtin/tool_loop.cpp#L248)、[schedule_tool.cpp:56](../../applications/assistant/schedule_tool.cpp#L56) | 通用 payload 转发/展示；ScheduleTool 迁移后保留待批、operation_id 和停后不登记行为 |
| 持久及恢复 | [call_records.cpp:200](../../core/work/call/call_records.cpp#L200)、[call_restore.cpp:203](../../core/work/call/call_restore.cpp#L203)、[call_store.cpp:13](../../core/kernel/service/persistence/call_store.cpp#L13) | 当前 CALL_STATE 是 type21/v1；扩展原契约、输入/结果及 journal/snapshot 共用 codec |

## 3 模块责任与接口形态

| 属主 | 拟承担的内容 | 不承担 |
|---|---|---|
| `core/catalog/capability` | 机器 Schema、类型/范围/单位/默认值、规范值编解码与静态投影形状；复用现有字段、UTF-8、目录 ID 校验 | 模型 JSON 语法、业务算法、任意 Schema 解释器 |
| `core/authority` | 复用 I64/ENUM/IDENTIFIER 的覆盖与完整 Grant 规则；定义固定 requirement 的逐项覆盖 | 从任意 text 推断目标、多个 Grant 拼接、隐含总量/关系策略 |
| `core/work/call` | 原始 Schema/投影证据、规范输入与结果、工具提议编码和保留/擦除生命周期 | 业务 Provider 内部算法、另一份权限来源 |
| Kernel | 编排暂存、准入、提交、复核、结果消费和查询 | 自建平行类型目录或把所有新字段塞入 Kernel |
| Provider/模型 adapter | 原生后端执行或模型协议格式解析，构造同一种公开值表示，调用共用语义校验 | 复制默认值/授权算法、逐能力手写 JSON 分支 |
| 扩展、策略、SDK、runtime | 定义业务与说明、使用通用值、提供必要便利函数、唯一装配 | 新 DSL、设备专用 Core 分支或万能 invoke(JSON) 旁路 |

拟增加模型中立 C99 值/Schema 头及只读访问、规范化接口，具体符号在实现时按现有命名落定。值采用 caller-owned 的节点表与字节视图，不跨 ABI 传 STL、yyjson/JSON 类型或递归指针：

- 节点显式区分 object、array、bool、i64/u64、enum/identifier、text、float64、decimal 和声明允许的 null；未使用的 union 字段必须为零。
- 节点表按前序排列。根与数组项不冒充对象字段；对象子项携带 Schema 稳定字段 ID。子树结束位置是显式的排他索引，标量结束于下一项；根覆盖整个表。对象内重复/未知字段、越界、交叉子树、不可达节点和非法深度均拒绝。
- Schema 本身也是有界非递归声明：对象字段、数组 item 形状及有限范围，不支持任意 `$ref`、递归类型、正则、任意 union 或回调执行的配置语言。
- 对象字段按稳定 ID 排序，列表保留顺序与重复项；不能按授权集合规则给业务列表去重。公开实现用有界显式栈，既不保留调用方可变输入，也不依赖无界递归。
- 注册复制机器 Schema/投影；说明仍留在扩展端口。模型工具视图在 submit 作用域借用，Provider 在返回前复制所需信息。策略结果视图只在 decide 中有效；查询复制到调用方缓冲。

纯规范化接口不访问 Kernel、I/O 或隐式全局状态，可对互不共享可变输入/输出的调用并发使用；调用者保持输入和 scratch 在返回前不变且存活。内部预检确定完整需求后再写结果，BUFFER_TOO_SMALL 返回所需容量，非法输入/容量失败不发布半个规范值。Kernel 注册、decide、submit/post 仍分别遵守现有 owner、回调作用域和可靠投递规则，纯函数可重用不等于 Kernel API 可重入。

## 4 数值、缺失与规范表示

| 类型 | 拟采用的语义 | 边界 |
|---|---|---|
| i64/u64 | 保留精确整数和原 ID/计数；各 Schema 声明有限范围及单位 | Schema 的完整范围不适合安全 JSON integer 时声明十进制字符串表示，不经 double 绕行；u64 投影 I64 的范围必须可精确容纳，否则拒绝投影注册 |
| float64 | 有限 IEEE binary64 近似值，Schema 声明范围；模型 JSON 解析采用 round-to-nearest/ties-to-even，输出往返同一 binary64 | NaN/Infinity、溢出及非零输入下溢成零拒绝，合法 subnormal 保留；负零规范为正零；不直接用于权限比较 |
| decimal | Schema 固定 scale 的精确十进制，内部为 I64 coefficient；例如 scale=2 时 1.2 与 1.20 均规范为 coefficient=120 | 不舍入；超 scale、系数溢出或不在范围内拒绝。初始 scale 上限候选 18，最终以具名公共限制及测量冻结 |
| enum/identifier/text | 显式有限枚举/目录标识符或有界 UTF-8 字符串，按 Schema 区分 | 不把任意路径字符串哈希或截断为授权 IDENTIFIER |

能力明确选择 float64 或 decimal，二者不互相隐式转换。模型协议中精确 decimal 使用带固定 scale 说明、不带指数的十进制字符串；adapter 必须检查原始词法，不能先变成 double 再声称精确。无效尾零可去除，超过 scale 的非零小数位拒绝；规范输出按声明 scale 生成。浮点舍入仅指该已声明近似类型的解析，不把精确输入偷偷降级为近似类型。金融计算、测量算法本身仍由能力实现负责，本包只传递、校验和保持数值语义。

缺失、默认与 null 不混用：required 缺失拒绝；optional 有声明默认值时在授权前填入，否则保持显式缺失；null 只有该字段允许时才合法，不等于缺失。默认值在注册时通过同一 Schema 校验，并属于 exact 契约，不允许 Provider 再施加隐藏默认值。未知/重复业务字段拒绝。未来 wire 信封的可选扩展规则与业务对象的封闭字段规则分别处理。

## 5 一个规范请求贯通授权与执行

```text
模型 JSON / 原生 C 值
  → 格式/形状检查 → 同一个 Schema 校验、默认填充与规范编码
  → 从规范值生成授权 actual → 固定 requirement/Grant 的完整覆盖
  → 原 Schema、原 Binding 和规范请求同批接受 → 持久派发屏障
  → 从同一保留请求复核权限/资源/依赖 → Provider 消费规范业务值
```

公开请求不再接受调用者另外提交一份可与业务数据不一致的 authority 参数。授权 actual 是当前调用的临时投影，Provider 执行使用的业务值才是单一输入事实。复核可从同一保留规范值重新生成投影，不重填默认值或依赖新目录。原生输入与编码输入共享语义验证器；不同信任/恢复边界调用同一验证器，不复制规则或新增 unchecked 入口。

### 5.1 投影范围

机器 Schema 将稳定业务字段路径映射到原授权域的 field_id、kind、unit；最多仍为既有授权维度数。首版路径允许穿过有界嵌套对象的固定标量路径，或同一个列表锚点的元素标量配合列表外固定标量。只有一层列表展开轴，不引入任意表达式、隐式跨列表配对、求和或关系推理；多层嵌套列表仍可作为普通业务数据，不能冒充已支持的多轴授权。

精确 decimal 可投影为 I64 系数，但缩放、授权单位、域边界与注册 Schema 必须精确对应；例如“0.01 元的整数单位”不能标成“元”后直接比较 coefficient。不得靠截断、容差或自动单位换算过授权。bool 若需权限维度，须显式映射为已声明的有限 enum 或整数值。float64 的授权路径在注册时拒绝，不能取整后继续执行。

授权域的 IDENTIFIER 仍受现 CatalogId 规则约束。文件路径、URI 等自由文本的权限模型不能由本包自动推导；能力需声明现有机制可以完整表达的目标/root/范围，表达不了时拒绝，不以 text 绕过。

### 5.2 列表采用同一完整授权逐项覆盖

用户已确认首版沿用完整授权并逐项检查：先固定一个匹配的 Goal/Task requirement 及其已有 Grant 引用，再逐项构造完整 singleton actual，全部满足才允许整次请求。不能对每个元素调用当前会“任选匹配 requirement”的整段 check_input，否则可能让元素分别选不同 Grant，形成拼接。

每项只生成“根标量 + 当前元素叶子”的有限字段视图，复用现有覆盖判定及路由检查。执行目标/外发目的地仍须符合同一 Binding；一个列表不能隐式 fanout 到多个 Binding。此处仅对准入整批允许/拒绝作保证，不承诺 Provider 的批量实际效果具有事务回滚性；部分完成仍用原 effect/UNKNOWN 与结构化结果表达。

作为授权锚点的列表要求 min_items≥1；空列表拒绝，不能因检查循环零次而自动通过。非授权业务列表可按其 Schema 允许为空。多个独立的普通业务列表仍可传递，但只有一个列表可作为该完整授权投影的锚点。元素关系、跨列表关联、整批总量等授权需求首版不支持，注册时明确拒绝；现 Scope 是独立字段区间/集合，不能声称其支持元组关系。

## 6 结果、模型提议与调用生命周期

- 普通工具的完成结果以声明的 result Schema 校验并规范编码，Task/查询提供对应只读或复制视图。文本是显式字符串 Schema，不保留无契约 legacy text 旁路。
- MODEL 正文可采用显式字符串 Schema，保留现 system/user 分离；模型最终文本与工具提议仍是不同的结果种类。工具提议携带精确 Binding/版本和业务值，不授予权限，不直接执行。
- 提议在可靠 post 边界检查其 Schema/形状和工具视图，策略回译时保留同一值语义；实际 request 再走同一个规范化/授权入口。权限和数据依赖在派发前仍会复核。
- Provider 的 FAILED、CANCELLED、ABANDONED 与 effect 分开保留；结构化 payload 不能把未知或失败伪装成完成。成功 payload 不合 Schema 时拒绝其 post，不能悄悄转成功、替换默认值或归还尚欠的终态借用。
- 本包迁移现有进度/查询/订阅/物理执行对原输入的读取，但不新增 AP-05 的模型订阅控制操作。查询返回仍遵守原 Task/owner、耐久与内容删除门控。
- 失败/部分完成是否携带业务结果，按声明的可选结果结构与现 outcome/effect 约束实现；ABANDONED/UNKNOWN 不携带可被当作成功使用的普通结果。错误 status/reason 不放任意后端原文或私有值。

## 7 历史解释、存储与兼容

当前调用记录实际是 CALL_STATE type21/v1，而不是 context/calendar 的记录号。它保存原 contract、authority domain、规范 input、result 及原 Binding；没有通用业务 Schema/投影。新记录必须保存恢复解释所需的有界机器 Schema、投影/精度/默认规则及 exact 版本，和原规范输入/结果一起保留；只有 Schema ID 或当前目录查询不足以证明历史语义。

沿原 CallRecord/CallStore 机制扩展，不为类型系统另建持久数据库。注册层的 exact 比较必须包括输入/结果 Schema、默认值、数值精度、授权路径与全部限制；恢复以记录中的原始事实校验，当前目录只有完全相同的契约才可继续未派发工作。已越过屏障的 UNKNOWN 不重演，不改 Provider、Binding 或 operation_id 掩盖未知。

用户已确认本包开发期直接升级：所有仓库内消费者一起迁移和重编译；若公共布局改变则增加实验 ABI minor，若记录布局改变则统一升级 storage schema。以本基线预计下一 ABI 为 0.48、schema 为 4，但实现前须检查号码是否已被其他交付占用；当前仍为 0.47/3，不能提前声称已发布。

旧目录在新写入前明确拒绝并保留，不读旧记录、不做迁移器、不清空、不回退旧快照。回退使用原二进制与原目录；开发使用新目录。能力语义版本、ABI、Core 存储、工具提议编码和 Native 外层文件各有自己的版本，不能只改一个版本号后推定所有格式兼容。

新增字节布局由记录属主的一套 reader/writer 定义，并同步 contracts/storage/records.md。journal/snapshot 复用同一调用 codec；原子配对、持久屏障、分页、合法 CRC 下的结构检查与首错传播保持。隐私擦除必须覆盖新增规范值、Schema 中可能含业务默认正文的片段、结果、工具提议、暂存和模型请求副本；不因新节点区留下内容副本。

## 8 消费者迁移清单

| 消费者 | 必须迁移的内容 | 保持的行为 |
|---|---|---|
| C99 API / C++ SDK / ABI smoke | 请求/结果/提议/工具视图/查询、Schema 注册、caller-owned 值访问及必要窄 helper；重编译所有布局消费者 | 当前 struct_size、owner/借用、无异常/RTTI 和错误规则；不新增 DSL |
| Catalog/Provider/Binding | 静态 Schema 与投影校验、原契约复制和 exact 匹配 | 不依注册顺序路由、不自动更换端点 |
| CallRuntime/CallTable | staging、规范值/原 Schema、准入、固定授权投影、派发复核及结果/查询 | 原 step/logical_call/operation 身份、资源准入和屏障 |
| 可靠邮箱及完成 API | typed 结果和提议的验证/复制，progress/terminal/control lane 的容量与 ACK | 取消不归还借用；同 attempt、晚到/重复结果处理 |
| OpenAI-compatible Provider | 通用嵌套 Schema、列表、两种数值和工具别名编解码，submit 内复制及 worker scratch | 不新增逐能力分支，保留 system/user 分离，不自动重试 |
| ToolLoop、规则与上下文 | 通用值转发、有界打印和结果输入下一轮；普通 MODEL 字符串 Schema | 不在策略内重写 validator、批准或业务参数解析 |
| ScheduleTool 及助理目标 | 结构化输入/结果；迁移提示说明和调用方后删除模型专用旧 parser | operation_id 找回、至多既有待批容量、默认待批、“停”后不登记；CLI 文本 parser 是独立入口，可构造同一类型后保留 |
| Mock、presentation、mock_execution、subscription | Schema 声明、payload 消费和完成结构 | 现正常行为、通知/物理停止事实和租期规则，不借机开新硬件实验 |
| runtime/common、Native/Sim、示例 | 配对描述、Schema 版本、容量、工具配置和各公开请求 | 唯一 composition root，保留确定性夹具与独立效果计数 |
| Call codec / journal / snapshot / replay | 原机器 Schema/投影、规范值和结果的统一新格式/预算 | 不重释旧调用，不创建旧格式 reader/迁移器 |
| 现有多节点 typed 夹具 | 仅同步嵌入的请求/结果表示 | 不把迁移变成完整 peer/MA-038 实现 |

不把全部 Goal、Task、Memory 参数一律改成业务树；只有实际嵌入新调用结构或自身格式随消费者迁移改变时才调整其记录和外层版本。

## 9 容量、复杂度与量测

现配置为 call_input 默认 4096 B、上限 131072 B，call_result 默认 4096 B、上限 65536 B，见 [ayni.h:207](../../include/ayni/ayni.h#L207)。它们不是新增 Schema、节点或模型 JSON 的免费容量。分别声明并计入：

1. Schema 节点/字段、总声明字节、深度、枚举/默认值；值节点数、单列表长度、总元素、单字符串及总正文、decimal 精度/scale。
2. 每 Call 的原 authority domain、业务契约证据、规范输入和结果；CallRuntime 的 staging/receipt、ProviderMailbox 的 progress/terminal 两条 payload lane。
3. 每模型请求的工具 Schema/说明副本、输入/结果节点及 JSON parser/writer scratch；单个工具及全部提议共享总预算，不能每项单独通过后总量溢出。
4. CallStore 的精确/最大记录长 → transition_batch_max → snapshot_record_max/payload_max → Host 日志/快照预留；不能只增加 call_input 常量。

初始工程候选：Schema 深度 8、每 Schema 128 节点、每值 256 节点、单列表 64 项、decimal scale≤18；业务声明还可收紧。它们不是实测平台支持声明，编码前须按最小/默认配置及正常最坏输入评估，转换为具名限额，并报告实际 arena/Provider 副本/栈和 step 观测。其他字节上限优先沿既有 caller 配置，不自动扩大。

所有边界计算先检查加法/乘法溢出和完整容量；禁止按深度无界递归，预计按节点线性遍历和对象规范排序有界处理。模型/存储 I/O 仍在 step 外；规范化、投影和纯回调须有有限工作量。容量不足明确拒绝且不留下部分规范输出、目录或已接受调用。

## 10 验证与实施顺序

以下是后续实现的验证安排，不是本轮已执行测试：

| 场景 | 需要证明的行为 |
|---|---|
| 纯软件批量计算 | 同时使用嵌套 options、items 列表、decimal 单价和 float 测量值；C 输入与本地 HTTP 模型 JSON 产生相同规范值和结构化结果 |
| 单一完整授权 | 固定同一 requirement/Grant；列表中一个资源越界整次不派发；不同 Grant 分别覆盖不同项仍拒绝；空授权列表拒绝 |
| 精度与默认值 | 对象换序、合法数字等价文本和默认填充按规定规范化；decimal 不经 double；float 不转权限整数 |
| 输入/结果边界 | 重复/未知字段、非法节点区间、深度/数量/总字节、空/非法指针、非有限浮点、超精度/系数溢出均有确定错误，失败保留原输出与调用状态 |
| 工具完整闭环 | 本地模型夹具 → 嵌套提议 → ToolLoop → 原治理链 → 软件 Provider → 结构化结果 → 下一模型轮；保留精确别名和消息角色 |
| 正常恢复与拒旧 | 沿用现正常重启及 schema 边界机制，原 Schema/输入/结果/绑定不被新注册重释，旧目录保留无新写 |
| 当前行为回归 | 现有生命周期、查询/取消、订阅、通知、私有性/删除、应用待批/停止、公开示例和既有 E1～E4 不被新格式破坏 |

建议一个完整实现 feature PR，以 focused commits 顺序组织：机器 Schema/值与纯 codec → 新请求/原契约/存储预算 → 授权/派发/完成/查询 → 模型与全部消费者 → 相关正常测试、文档、自查与独立审查。只有兼容边界或可审查复杂度确有必要时再拆独立可交付 PR，不先合入无法使用的半套新旧格式。

本轮仅做静态盘点、设计和文档检查；后续实现本地按影响选择验证，日常完整 ASan/UBSan 由 CI 承担。ABI/容量实际变化后测 arena，未明确恢复的新故障/恢复、变异专项继续暂停；现有相关回归保留。阶段收尾不得把未验证的必需保证写成已通过。

## 11 已定事项与尚需冻结的实施细则

用户已确认：首版含列表/嵌套及两种小数；逐项由同一完整授权覆盖、拒绝空授权列表，关联/跨列表/总量另行扩展；权限数值只用整数/精确十进制，浮点不直接参与；开发期直接升级消费者，拒旧保留目录，无迁移器。

实现前还要冻结具名容量、C99 字段布局、规范字节布局、错误码复用/必要新增、各模型 codec 的支持子集及精确结果访问方式。这些属于实施设计，不能用未测候选值声称平台支持。若实证要求改变已定精度、授权、兼容或功能范围，再带消费者影响和选项请用户决定。
