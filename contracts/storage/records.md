# 持久记录格式

- **关联**：计划 §2.8、§4.2「持久化」、§4.3、§4.4「升级」；ADR-0002、ADR-0016、ADR-0026、ADR-0027、ADR-0030、ADR-0031、ADR-0096；`MA-010`、`MA-006`；E2 F0–F8、F11、F12，E4 步骤 6
- **属主**：`durability`（批次编码、持久前缀、快照、重放、栅栏记录、迁移机制）；领域记录的内容由各属主模块定义，`durability` 不理解其语义
- **状态**：M1 当前批次与快照只读写 `storage_schema=11`（ADR-0110）；其他 schema（包括 0～10） 在新写入前只读受限并保留，不迁移、不自动清空或回退（ADR-0038 决策 6）。布局以本文件和编解码测试为准；Native/模拟证据不代表 ESP32 或真实 OTA 验证；C 的 Grant/完整命令扩展及 D 的撤销扩展各自验证与交付状态见 ADR-0030/0031；D 已通过 Native 场景、覆盖率与内存量测，#28 已合并

## 编码约定

- 全部整数为定长小端序；字节串以 `uint32` 长度前缀；不持久化 C++ 内存布局（C-VER-023）。
- 校验和为 CRC-32/ISO-HDLC（与 ESP32 ROM 的 `esp_rom_crc32_le` 相同，Native 由 `foundation` 实现同一算法）。选它是因为 ESP32 可直接用 ROM 实现，不增加固件体积；CRC 只用于发现损坏，不提供授权或防篡改（C-STORE-008）。

## 日志批次

一个提交批次是一个 transition 的全部记录（`domain/commit_and_output.md`）。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYJB` |
| 4 | 2 | `storage_schema` | 存储格式版本 |
| 6 | 2 | `header_length` | 严格为 44，含末尾的 `header_crc`；不接受扩展头 |
| 8 | 4 | `writer_epoch` | 写入方代际（C-STORE-005） |
| 12 | 4 | `flags` | 保留，写 0；读到未知位时进入 `RESTRICTED` |
| 16 | 8 | `commit_seq` | 连续递增 |
| 24 | 8 | `transition_id` | 重试同一 transition 时不变（C-COMMIT-008） |
| 32 | 4 | `record_count` | 记录数 |
| 36 | 4 | `payload_length` | 记录区字节数 |
| 40 | 4 | `header_crc` | 位于偏移 40，覆盖偏移 0 至 39；先 CRC、再判格式 |
| 44 | `payload_length` | 记录区 | 见下表 |
| — | 4 | `commit_magic` | ASCII `AYJC` |
| — | 4 | `commit_seq_low` | `commit_seq` 的低 32 位，把提交标记绑定到本批次 |
| — | 4 | `payload_crc` | 覆盖记录区 |

Host 通过 `include/ayni/storage_format.h` 的 `ayni_storage_read_batch_identity`
提取前 32 字节的身份（ABI 0.28，ADR-0040）；该纯函数不校验 schema、CRC、
marker 或身份语义，不能替代 Core 的完整批次校验。失败不修改输出。ABI 0.51 的 `ayni_storage_read_schema` 只读取 AYJB/AYSN 六字节前缀的原始 schema，不校验 CRC/激活关系。Native 在目录锁内先只读检查已有 log 与两个 snapshot，拒绝异版后才允许外层 SQLite 创建；完整记录与恢复合法性仍由 Core 验证。

每条记录：

| 长度 | 字段 | 说明 |
|---:|---|---|
| 2 | `record_type` | 见“记录类型” |
| 2 | `record_version` | 该类型的版本 |
| 4 | `record_flags` | 位 0 = `REQUIRED`：所有已知类型必须置位；未知类型/版本无论此位均拒绝；其余位保留，未知位拒绝 |
| 4 | `length` | 记录体字节数 |
| `length` | 记录体 | 由属主模块定义的字段编码 |

schema 11 的 `REQUIRED` 是已知记录合法性的必需标记，不再用它决定是否跳过未知记录；未知记录始终拒绝。

## 记录类型

名称、所属批次与属主在此固定；类型编码在实现该记录的工作包中分配，记入下表。

| 记录 | 属主 | 所属批次 |
|---|---|---|
| `BOOT_EPOCH`（类型 1，版本 1，置 `REQUIRED`；正文为 8 字节小端 `boot_epoch`） | `kernel` | 单独批次，开放输出前提交（C-ID-009、ADR-0016） |
| `WRITER_FENCE`（类型 10，版本 1，REQUIRED；正文 4 字节小端新 writer epoch，须与头部相等） | `durability` | 单独批次（C-STORE-005） |
| `OWNER_BINDING`（类型 12，版本 1，REQUIRED） | `authority/principal` | 首次所有者绑定，与启动领域 transition 同批；已绑定后不重复写入（ADR-0028） |
| `INSTANCE`（类型 2） | `work/agent` | 启动或改变实例的 transition 批次（ADR-0026） |
| `GOAL`（类型 3）、`GOAL_STATE`（类型 4） | `work/goal` | `GOAL` 在创建它的批次（B1）；`GOAL_STATE` 在每个改变该 Goal 的批次，含 occurrence 游标（B2），所以不另设 `OCCURRENCE_CURSOR` |
| `GOAL_AUTHORITY`（类型 17，版本 1，REQUIRED） | `work/goal` | 创建/批准/暂停/重新授权由正文 phase 和 GOAL_STATE.change 配对，与直属活 Task 及清理事实同批；每批最多一个 Goal（ADR-0037、0038） |
| `TASK`（类型 5）、`TASK_STATE`（类型 6） | `work/task` | `TASK` 在派生它的批次（B2）；`TASK_STATE` 在每个改变该 Task 的批次，含 `TASK_PROGRESS` 的内容 |
| `TASK_AUTHORITY`（类型 18，版本 1，REQUIRED） | `work/task` | 派生 Task 时与确切 `TASK`/`TASK_STATE` 同批，状态后紧接准入事实（ADR-0038） |
| `WORK_AUTHORITY_CLEANUP`（类型 19，版本 1，REQUIRED） | `work/authority` | 工作清理请求/一步的批次末尾；快照固定一项，含明确 idle（ADR-0038） |
| `TASK_STEPS`（类型 7）、`TASK_CHECKPOINT`（类型 8） | `work/task` | 提交步骤或私有检查点的决策批次（B3），即 `STEP_CHECKPOINT` |
| `COMMAND_MAPPING`（类型 9，日志 v1/快照 v2，REQUIRED） | `kernel/command` | 接受该命令的 transition 批次（B1）；正文始终保存 effect、内容标记、canonical/result/来源长度；精确命令携完整请求和必要原结果（ADR-0030、0031、0038） |
| `BUDGET_RESERVATION`、`BUDGET_SETTLEMENT` | 已废止（ADR-0042），未分配类型 | 不实现费用账本 |
| `CALL_IDENTITY` | `work/call` | B3 |
| `ACTION_STATE`（20/v1 codec，嵌入 CALL_STATE，不单独写 journal） | `work/action` | 通过 REQUIRED CALL_STATE 保留；准备、屏障与终态各自的批次，终态在 B4 |
| `CALL_RESULT` | `work/call` | B4 |
| `MEMORY`（23/v1）、`CONTEXT`（26/v1），REQUIRED | `knowledge` | 管理/策略变更，见 P3 记录布局 |
| `PROVENANCE`（24/v1）、`PRIVACY`（25/v1），REQUIRED | `work/provenance`、`work/privacy` | 同批来源与独立清理进度，见 P3 记录布局 |
| `GRANT_CREATE`（类型13，版本1，REQUIRED） | `authority/grant` | Grant 创建，与当前完整命令身份/结果同批（ADR-0030） |
| `GRANT_STATE`（类型14，版本1，REQUIRED） | `authority/grant` | lifecycle 区分到期、后代撤销、专属 Goal 结束，始终保存 revocation root；到期/结束要求工作清理，不持久 Gate |
| `GRANT_REVOCATION`（类型15，版本1，REQUIRED） | `authority/grant` | 显式根撤销或明确 no-op，与完整命令结果同批；要求工作清理 footer（ADR-0031、0038） |
| `GRANT_REVOCATION_PROGRESS`（类型16，版本1，REQUIRED） | `authority/grant` | 根cursor与本步后代状态（如有）同批B5（ADR-0031） |
| `INVALIDATION_CURSOR` | 已由 PRIVACY 25/v1 表达，不另分配类型 | B5 |
| `SNAPSHOT_COMMIT`（类型 11，版本 1，REQUIRED；正文见下） | `durability` | 快照切换 |

## 动作事实正文（ABI 0.31 嵌入调用记录）

`ACTION_STATE` 20/v1 的字段顺序固定，均为小端；固定大小 84 B 加 Binding ID。0/8 为 operation ID / logical call ID（u64）；16/20/24/28 为 Binding version / phase / effect / evidence（u32）；32/40/48 为 prepared 的 boot / transition / commit seq（u64）；56/64/72 为 latest 的对应三项；80 为 Binding ID 字节数（u32），84 起为 ID 内容（1..32 B）。没有 submit claim、指针、时钟或物理占用。

phase 1..7 依次为 prepared、dispatching、in_progress、completed、failed、cancelled、result_unknown；0 仅表示内存原位构造，禁止持久。effect 1..4 为 none、partial、complete、unknown；evidence 0..4 为 none、local_non_delivery、provider_result、authoritative_result、authoritative_not_accepted。合法组合由 `ActionRecord::valid` 唯一校验，codec 复用；权威未受理只能配 FAILED/NONE。准备和最新 stamp 形状、顺序以及 operation 创建代际须一致。跨表调用/Binding/原参数、查询与尝试关联通过 CALL_STATE 及 Task/命令配对校验；codec 正常往返不等于完整恢复专项证据。

## 调用输入正文（ABI 0.31 嵌入调用记录）

`work/call` 的内部 input format 1 使用有界 typed 授权参数及 UTF-8 文本。外层 CALL_STATE 的原 CallContract 携带独立 input format 版本。正文布局：参数规范字节数 u32、文本字节数 u32、完整参数、文本。固定头为 8 B；没有指针、NUL 终止要求或 JSON。参数使用现有 domain format 1，但每个已注册字段必须为单值且全部提供；授权与 Provider 消费须使用这同一份参数。文本最大实现界限 65536 B，具体调用上限由调用目录/配置共同决定，超限明确拒绝。U+0000 可作为长度限定的普通字符；拒绝无效 UTF-8、过长编码、代理区及超出 U+10FFFF 的码点。

此 codec 只证明结构与原域相符，不证明 Grant、使用同意、当前 Binding 或业务持久化。正文保留与删除按 ADR-0042 补充决定执行，密钥、原始媒体和诊断日志不使用该存储路径。相关局部正常测试见 `CallInputTest`；重启继续与删除尚未验证。

## 所有者绑定记录

`OWNER_BINDING` 正文为 16 字节：`principal_id` u64、`binding_revision` u32、保留位 u32（必须为 0），身份和绑定版本均非零。绑定只建立一次；重复、非法记录或缺失 REQUIRED 标志按损坏处理，即使快照已覆盖该历史记录。Host 提供的身份或绑定版本不匹配不属于日志损坏：在任何新恢复写入前，以 `OWNER_BINDING_MISMATCH` 进入只读诊断，保留校验后的旧对象。无绑定的旧存储须核对现存 USER Goal 来源的 principal/revision 及命令映射 principal；任一与 Host 冲突也受限，不隐式转移所有权。首次绑定持久前不开放认证会话（ADR-0028）。

## 工作记录

schema 11 的Call区分首次完整v2与后续状态v3，见下文（Tier 0 `EPHEMERAL` 调用仅驻留运行期 RAM，不写入日志或快照，见 ADR-0110）；其余类型/上下文保持原当前正文。下表为日志 v1；快照中 INSTANCE/OWNER_BINDING/GOAL/TASK/GRANT/COMMAND_MAPPING 使用 v2（追加原历史）。其余记录在允许的两种上下文均为 v1。状态记录写完整事实，不重执行业务规则。ID 字节串至多 32 B；固定长度包括所有长度/count 字段，不含变长数据。

| 类型 | 记录 | 当前正文 | 固定长度 |
|---:|---|---|---:|
| 2 | INSTANCE | agent_id、instance_id（u64），revision、lifecycle（u32） | 24 |
| 3 | GOAL | 五个 u64：goal/parent/owner/replaced/origin principal；五个 u32：origin kind/channel/binding revision、depth、GoalType version；GoalType ID 长度串；trigger kind u32、delay/period u64、source 长度串；expiry/wait duration u64；参数长度串；profile kind u32/recovery u32/period u64；pause_group、authority_required、provenance_required（u32）；tool mode/count（u32）及 count 项 Binding version/role/ID 长度串 | 144 |
| 4 | GOAL_STATE | goal/last_task u64；lifecycle/revision/cancel_revision/skipped/skip_reason/taken/waiting/flags（u32）；change u32 | 52 |
| 5 | TASK | task/goal/owner/replaced（u64），occurrence/count（u32），calendar_local/utc（u64），calendar_present/provenance_required（u32） | 64 |
| 6 | TASK_STATE | task_id u64；lifecycle/revision/cancel_revision/hold_status/hold_reason/step_count（u32）；model used/limit/blocked（u32）；selected call u64/end u32；wait call/generation/sequence（u64）/phase/form（u32）、boot/gaps（u64）、cancel_requested/cancel_result/outcome/timeout_ms（u32）；call_unknown/change/wait_present（u32） | 132 |
| 7 | TASK_STEPS | task_id u64、first/count（u32）、count 个 key 长度串 | 16 |
| 8 | TASK_CHECKPOINT | task_id u64、writer version u32、writer ID 和状态长度串、provenance_required u32 | 24 |
| 9 | COMMAND_MAPPING | principal/command/digest/floor（u64），intent/object kind（u32），object id u64/revision u32；content/effect/canonical_format（u32），canonical/result 两个长度串，来源列表 | 76（空来源） |

GOAL_STATE flags 位 0=next scheduled，位 1=expiry pending，其余拒绝；change 依次 1=普通、2=授权、3=清理、4=授权暂停、5=重新授权。TASK_STATE.change 依次 1=普通、2=授权、3=重新授权、4=提高模型上限、5=恢复等待。change 表示必须验证的事务证明，不能通过改成普通标记绕过完整配对。快照状态的 change 必须为 1。

布尔标记仅允许 0/1。Goal 来源要求授权伙伴；pause_group=0 表示无组，其他值按固定范围验证。没有 calendar 的 Task 两个时间必须为 0，解码为 TIME_NONE。没有 wait 的 Task 全部 wait 字段必须为零。有 wait 的生命周期只能 WAITING 或其用户 PAUSED；selected call/end 为零，跨表核对同 Task 的 action/订阅、形态及代际/前沿；deadline_us 不编码。call_unknown 是独立的 Task 阻塞，不能覆盖策略/资源暂停；同批 Call 设置、最后一项 UNKNOWN 对账解除，前台完成不得解除其他来源。

日志 `TASK.provenance_required` 必须为 0，置 1 按损坏处理；快照 TASK 则按保留来源设置此位，置 1 要求完整的来源 footer。两种上下文共用创建字段 codec，约束由各自的恢复入口执行。

- 记录不含单调时间（C-STORE-014）：Goal 写时长，不写到期时刻；Task 的 `due_us` 不写入。现有Goal/Task计时从恢复结束时刻起按完整时长重新计算（ADR-0026 决策6）；Grant限时恢复直接失效，绝不重锚（ADR-0030）。
- 一个批次内的次序：可选的首次 `OWNER_BINDING`，`INSTANCE`，D显式根`GRANT_REVOCATION`，`GRANT_CREATE/GRANT_STATE`，D根`GRANT_REVOCATION_PROGRESS`，新建 Goal 的 `GOAL`，各 `GOAL_STATE`，新建 Task 的 `TASK`，各 `TASK_STEPS`、`TASK_CHECKPOINT`，各 `TASK_STATE`，最后 `COMMAND_MAPPING`。新建的 Goal 或 Task 须在同一批次带有状态记录，否则为损坏。
- 状态记录引用的对象须已在表中，Goal 与 Task 的 `revision` 不减，`TASK_STEPS` 的 `steps_before` 须等于 Task 已有的步骤数，取代的对象须已终结；Goal 的 owner 须是此前记录启动的实例，终态的 Goal 与 Task 不再改变，带 `hold_status` 停下的 Task 只能结束，实例的 `revision` 不减、停止后不再改变；日志末尾不得有未终结的 Task 留在终态 Goal 之下，也不得有未终结的 Goal 留在终态的父目标之下或属于已停止的实例（结束 Goal 的 transition 同时结束它的后代与 Task，停止实例的 transition 同时结束它的 Goal）。违反者为损坏（C-STORE-009）。新建对象取代的对象记在 `GOAL`、`TASK` 中，重放按记录取代，不重新选择。本次启动的容量或限值放不下的记录使恢复以 `STATE_EXCEEDS_CAPACITY` 受限，损坏或容量失败受限时丢弃已重放的内容；所有者绑定失配保留已校验的诊断模型，不写入新批次（ADR-0028）。
- 不改变持久状态的 transition 写一个空批次，`commit_seq` 仍然连续。

## Grant 与完整授权命令记录

Grant 当前日志 type13/v1 总固定 92 B，加 Capability ID、source domain、scope 和 business_contract 的正文。顺序为 grant/replaced/subject/parent/owner（u64），binding_revision/capability_version（u32），Capability ID 长度串，lifetime/delegation（u32），duration u64，depth u32，domain/scope 两个长度串，bound_goal_id u64，独立 business_contract 长度串。绑定 0 表示普通；非零必须早于 Grant ID、根且不可委派。原域必须覆盖保存 Scope，父链只能收窄。

GRANT_STATE type14/v1 固定 24 B：id u64、revision/lifecycle u32、revoked_by_root_id u64。后代 REVOKED 必须指向合法的另一根；EXPIRED/GOAL_ENDED 必须 root=0 且同批有工作清理 footer。revision 必须为终态 2。GRANT_REVOCATION type15/v1 固定32 B：id u64、revision/effect u32、after/through u64；GRANT_REVOCATION_PROGRESS type16/v1 固定24 B：root/after/through u64。根撤销与 exact revoke 命令及请求 footer 配对；后代终态与同批根进度配对。固定 through、单调 after；禁止同批未配对身份被下一条替换。

COMMAND_MAPPING.content 为 1=retained、2=erased。canonical_format 非空为1、空为0；canonical/result 长度和来源列表始终编码。普通命令可仅保存 digest，其 retained 正文的 effect 必须为 CHANGED，其他 effect（含未知值）按非法正文拒绝；exact 命令由 kind 选择对应业务请求 codec，核对 digest、对象、effect 和必要原始结果。擦除仅快照允许：digest=0、canonical/result/来源为空、重试返回 EXPIRED。它不是旧版本回退。

批准、重新授权仍保存完整原请求和不可变结果，不从当前引用重建。授权结果固定52 B：establishment transition、binding seq、parent Goal/parent seq、input incarnation/through（六个u64），requirement_count u32，再按原顺序各 grant_id u64/revision u32。批准创建精确缺项数量的专属根，保留旧引用；重新授权在一个封闭批次中覆盖 Goal、全直属活 Task、原始双 CAS 和请求 footer。缺项、重复、额外工作或跨批伙伴均损坏。请求的 Goal echo 保留原类型/参数/trigger/日期规则/暂停组。

Canonical Grant create 保留原 raw Scope 表达、expected revision、Capability/version、subject/parent/lifetime/flags/duration；不是展开后的 domain。canonical revoke 固定16 B（kind/expected u32、target u64），已受理 expected 只允许1/2，effect CHANGED/NO_CHANGE/TARGET_GONE 对象 revision 分别2/2/0。先前受理的 expected2 在 reload 后仍可作用于 ACTIVE1，不能再次检查首次 CAS。

持久事实不依赖当前 catalog。缺 REQUIRED、未知格式、非法正文、原始所有者或精确命令配对失败均拒绝；配置不足报告容量，不截断。Grant 只保存 duration，限时 Grant 恢复失效，不重锚。

## 控制记录与读取

- `WRITER_FENCE` 单独成批，新 epoch 必须严格大于前缀现有有效 epoch。其后的完整旧 epoch 帧被忽略，不推进有效 commit、transition 或 boot 基线；其他有效批次仍须连续。
- `SNAPSHOT_COMMIT` 正文为 `slot` u32、`payload_crc` u32、`generation` u64、`covers_commit_seq` u64、`max_transition_id` u64，共 32 字节。它自身是覆盖边界之后的下一批次；维护也消耗统一 transition 身份。当前外壳只接受 schema 11；无旧格式 reader、迁移或自动清空。
- 恢复使用 Host 物理 cursor，不能用最大已见 commit_seq 翻页；旧 epoch 的高序号帧可能位于合法新批次之前。cursor 在同一日志实例中严格递增，轮转后从 0 重读。
- 首个完整批次放不下时，Host 以读专用 `READ_BUFFER_TOO_SMALL` 报精确所需长度，映射 `STATE_EXCEEDS_CAPACITY`。真正 I/O 故障、非法大小或 `OK` 越界仍是 `STORAGE_READ_FAILED`。

## 快照

快照采用两个槽。维护先冻结新 transition 并排空 journal，得到边界 `S`，再写非当前槽的内容，回读校验版本、长度与 CRC 后独立写槽尾标记。槽尾标记只证明槽内容完整；日志中的 `SNAPSHOT_COMMIT` 是激活新状态的唯一切换提交，持久化后才轮转旧日志。尚未激活的新槽不参与恢复。

### 槽头与槽尾（schema 11）

头部固定为 72 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYSN` |
| 4 | 2 | `storage_schema` | 5 |
| 6 | 2 | `header_length` | 72 |
| 8 | 8 | `generation` | 从 1 起，每次激活递增 |
| 16 | 8 | `covers_commit_seq` | 快照包含的最后一个提交 `S`；初始空快照为 0 |
| 24 | 8 | `max_transition_id` | 边界 `S` 处最大的有效 transition；不计被 fence 排除的旧写入方 |
| 32 | 8 | `boot_epoch` | 边界 `S` 处最后持久化的启动 epoch；初始空快照为 0 |
| 40 | 4 | `writer_epoch` | 边界 `S` 处有效的写入方代际；初始为 1 |
| 44 | 4 | `flags` | 保留，须为 0 |
| 48 | 8 | `payload_length` | 正文字节数 |
| 56 | 4 | `record_count` | 正文记录数 |
| 60 | 4 | `payload_crc` | 覆盖全部正文，可跨页累计 |
| 64 | 4 | 保留 | 须为 0 |
| 68 | 4 | `header_crc` | 覆盖头部前 68 字节 |

正文紧随头部，其后的独立槽尾固定为 24 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `commit_magic` | ASCII `AYSC` |
| 4 | 8 | `generation` | 与槽头相同 |
| 12 | 4 | `payload_crc` | 与槽头相同 |
| 16 | 4 | `header_crc` | 与槽头相同，绑定全部头部元数据 |
| 20 | 4 | `marker_crc` | 覆盖槽尾前 20 字节 |

读取时先校验头部 CRC，再判断 schema 与 flags；不支持的版本或位进入只读诊断。`generation`、`writer_epoch` 不为 0，`boot_epoch` 不超过 ID 的 32 位 epoch 上限，`max_transition_id` 不超过 2^63−1；初始 `covers_commit_seq=0` 的快照必须为空且 transition 与 boot epoch 均为 0。

### 静态领域记录

正文使用日志的12 B信封；上下文严格分类，不猜长度或重用历史版本。

| 类型/版本 | 当前快照正文 | 长度 |
|---|---|---:|
| 12/v2 OWNER_BINDING | 日志正文 + 原 establishment transition/commit（u64） | 32 |
| 2/v2 INSTANCE | 日志正文 + 原最新 transition/commit（u64） | 40 |
| 3/v2 GOAL | 完整创建 + GOAL_STATE（change=1）+原 creation transition/commit、current commit（u64） | 创建+76 |
| 5/v2 TASK | 完整创建 + created transition/current commit/steps seq/checkpoint seq（u64）、steps_before u32 | 100 |
| 13/v2 GRANT | 完整创建+created/last stamp（各16 B）+revision/lifecycle（u32）+revoker/after/through（u64）+cleanup stamp（16 B） | 创建+80 |
| 9/v2 COMMAND_MAPPING | 完整日志正文+原 acceptance commit_seq u64 | 日志+8 |
| 7/8/6/18 v1 | 每条 key、可选检查点、完整 state（change=1）、紧邻准入 | 各属主当前大小 |
| 17/v1 GOAL_AUTHORITY | 完整原授权事实 | 72+每需求32+ID/域/Scope/业务证明正文 |

其余允许快照的记录使用与日志相同的完整当前正文；Grant state/root progress、Goal state、BOOT/FENCE/ACTIVATION 不单独导入快照。

顺序为可选的单例 `OWNER_BINDING`、全部持久实例（不含 `instance_id=0` 的本次注册项）、按父先子后深度排序的Grant、按深度从小到大的 Goal（有 authority 标记后紧接 type17，可跨页）、每个 Task 的元数据/逐条步骤/检查点/状态/准入（type18，紧接状态且允许跨页），最后按原接受 `commit_seq` 递增的命令映射。静态导入允许终态父子与已经不存在的 `replaced_goal_id`、`replaced_task_id`、`replaced_grant_id`前驱，仍检查 ID 唯一、owner、父子深度、版本、游标、状态与最终跨表一致性。原始创建身份、最新提交、步骤提交及检查点提交均保留，不改写成 `S`，以保持 READY 顺序、终态回收顺序与命令重试结果。

所有对象与命令的提交不超过 `S`，创建 transition 不超过快照基线；本次注册内容的缺失导致的 Agent fault、Goal hold、Task recovery hold 不写入快照。没有注册的 principal 仍占有原有的一行命令窗口及下限；第二个 principal 的持久命令放不下时明确报容量不足，不丢弃原行。命令窗口缩小时按原接受次序让位并提高下限。单调时刻不写入：Goal 只保留时长及是否待发生，Task不写`due_us`，这些工作计时最终恢复后重锚；Grant只写duration，恢复限时Grant失效，不能重锚。Grant保留created/last/cleanup stamp及终态顺序；三个stamp分别检查commit不超过S、transition不超过max_transition_id，非零cleanup独立检查，EXPIRED的last不能靠created界限替代。正常命令窗口可已淘汰创建mapping，snapshot不要求每个保留Grant仍有原mapping。

所有auth mapping核对OwnerBinding principal及接受seq、合法Kernel ID/boot，create继续校验完整不可变创建事实与scope。retained revoke CHANGED target必须为self-root REVOKED、终态seq等于mappingseq；NO_CHANGE target必须已终态且终态seq严格小于mappingseq；TARGET_GONE target必须缺席。已合法回收的CHANGED/NO_CHANGE target允许缺席。被根清理的保留后代ID不得超过根after，terminal stamp不得超过根cleanup stamp；revoker/after/through同样核对合法Kernel ID及snapshot boot界限；不改写为snapshot边界冒充原事实。

导出、回读与恢复按页复用同一存储缓冲，单条记录上限由配置容量计算，整份快照可以大于缓冲；Core 不分配另一份 WorkModel 或完整快照内存。任何损坏、不支持或容量不足均丢弃本次导入的全部可恢复状态，自动reload已接受的易失Gate/pending仍由各owner保留。绑定不匹配时，已完整校验的模型仅保留用于只读诊断。

### 事务与容量

普通与专属 Grant 共用完整创建/清理表示，绑定始终显式。Goal approval/re-authorization 的 exact command 及原结果随快照保留；所有状态、stamp、boot 和原始请求一致性检查继续有效。每条记录由原不变量属主编解码；Kernel 维护跨记录配对；durability 仅负责封装、CRC、前缀和激活。

容量使用当前完整正文的 owner size/max，来源列表、授权结果、日历、订阅、execution 和擦除标记都计入原有 arena、journal 与分页快照预算；不新增完整模型副本或旧格式暂存区。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-001 | 批次按上表编码：头部、记录区、提交标记；`header_crc` 位于 `header_length − 4` 并覆盖其前的全部头部字节；`header_length` 小于 44 时批次不完整；`header_crc`、`payload_crc` 与 `commit_seq_low` 都校验通过才算完整 | 只检查头部 CRC，记录区短写的批次被当作完整 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_unsupported`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-002 | 只有提交标记完整的批次可见；日志末尾不完整的批次被忽略，不部分应用。忽略尾部由 Host 负责（C-HOST-009）：Kernel 把读到的每个批次当作已提交，未通过校验的批次按 C-STORE-009 处理（ADR-0016） | 尾部批次只写了一半记录，恢复时应用了前半 | M1 已实现：Kernel 侧 `BatchCodec.every_short_write_is_corrupt`、`PrefixScan.damaged_batch_stops_the_scan`；Host 丢弃撕裂的尾部 `HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.torn_tail_is_truncated_before_the_next_append`；追加中断电的批次只在完整写入时计入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；重放受限时丢弃已重放的全部内容 `WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`（ADR-0026）；E2 F0、F7、F8 |
| C-STORE-003 | `commit_seq` 从 1（有快照后从快照覆盖的下一个）起连续递增。Host 只交回完整批次，所以读到的缺口或与 Host 分帧不一致的 `commit_seq` 都是已提交区域损坏，按 C-STORE-009 处理（ADR-0016） | `commit_seq` 7 缺失，恢复跳过它继续应用 8 和 9；或把前缀截断在 7 之前，下一次 append 又写出一个 7，日志分叉 | M1 已实现（物理读取、连续前缀与栅栏）：`PrefixScan.commit_seq_must_continue_exactly`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`、`WriterFenceScan.fence_must_advance_the_writer_and_match_its_header`、`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；ADR-0027 |
| C-STORE-004 | 未知记录类型/版本、未知 flags、不支持的 schema/头长度一律 RESTRICTED；已知类型缺 REQUIRED 为损坏，不跳过未知 optional | 新固件写入的授权记录被旧固件跳过，旧固件按更宽的权限运行 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_are_rejected_regardless_of_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_restrict_without_writing`、`SnapshotFaults.an_unknown_required_record_in_a_covered_batch_still_stops_recovery`、`KernelLifecycle.an_optional_record_of_a_newer_version_is_rejected`；E4 步骤 6；C-VER-020 |
| C-STORE-005 | 受限栅栏确认旧写入方隔离后，写入 `WRITER_FENCE` 记录（含新 `writer_epoch`）；恢复时忽略栅栏记录之后出现的旧 epoch 批次 | 旧 flush 在重载后落盘，恢复时被当作新批次应用 | M1 已实现（Native 排空、Sim epoch 安装、旧写拒绝和物理扫描）：`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`KernelFence.unavailable_isolation_preserves_diagnostics_without_new_boot`、`SimStorageFaults.fence_rejects_an_owned_late_write_after_its_unknown_completion`、`HostStorage.writer_fence_isolates_append_snapshot_and_trim`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`、`KernelFence.isolation_below_the_epoch_asked_for_restricts`；动作并存与执行端停止随 `MA-022`；E2 F12 |
| C-STORE-006 | 快照先写内容、回读校验后独立写槽尾标记；双槽与 `generation` 保留旧有效内容，只有绑定该槽的 REQUIRED `SNAPSHOT_COMMIT` 持久化后才激活并轮转。恢复只认最新有效激活绑定的槽，目标损坏或不支持时不回退旧槽 | 新槽内容尚未完整即覆盖唯一旧槽；或最新激活的槽损坏后按旧槽继续执行 | M1 已实现（槽格式、激活/轮转和发布阶段断电）：`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；`SnapshotFaults.power_loss_at_each_publication_request_keeps_the_durable_work`、`SnapshotFaults.corrupt_or_unsupported_active_slot_never_falls_back_to_an_older_slot`、`SnapshotFaults.a_candidate_can_replace_a_superseded_slot_before_an_older_trim_finishes`、`SnapshotFaults.the_snapshot_after_a_recovered_one_takes_the_next_generation_and_slot`、`SnapshotCodec.an_activation_reads_back_only_what_a_writer_can_store`、`SnapshotCodec.boot_epoch_beyond_the_id_epoch_is_corrupt`；C-HOST-011 |
| C-STORE-007 | 恢复从最新激活绑定的快照开始，重放 `commit_seq > covers_commit_seq` 的已提交批次；同一 transition 不重复应用（C-COMMIT-002），对象原始 stamp、READY 顺序、回收顺序、命令结果与下限不被快照覆盖提交改写 | 快照已包含的批次再次应用，或全部对象改用同一覆盖提交，改变任务公平性与回收选择 | M1 已实现（现有工作模型的静态分页快照与尾部重放）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`WorkSnapshot.paged_round_trip_keeps_the_original_stamps_and_checkpoint`、`WorkSnapshot.ready_and_reclamation_choices_match_the_uncompacted_model`、`WorkSnapshot.terminal_parent_and_child_keep_their_reclamation_order`、`WorkSnapshot.a_retained_object_can_name_a_predecessor_already_reclaimed`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`、`WorkSnapshot.smaller_object_and_checkpoint_capacities_report_no_room`、`WorkSnapshot.smaller_goal_parameter_limit_is_capacity_failure`、`SnapshotFaults.a_snapshot_record_beyond_the_buffer_exceeds_capacity`、`WorkSnapshot.object_stamp_beyond_the_snapshot_boundary_is_corrupt`；其余领域随各自工作包；E2 F11 |
| C-STORE-008 | CRC 只用于发现损坏，不作为授权或防篡改依据 | 以 CRC 正确为由信任一条从外部导入的 Grant 记录 | 评审 |
| C-STORE-009 | 已提交区域（不是末尾的不完整批次）出现损坏时进入 `RESTRICTED`，不跳过损坏批次继续应用后续批次 | 中间批次 CRC 错误，恢复跳过它，导致后续撤销记录引用不存在的 Grant | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`SnapshotFaults.payload_damage_the_records_survive_is_still_corrupt`；M1 已实现（工作记录违反表规则或日志末尾不一致时为 `LOG_CORRUPT`，已重放的内容全部丢弃，ADR-0026 决策 5）：`WorkRecovery.log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt`、`WorkReplay.goal_created_without_its_state_is_corrupt`、`WorkReplay.live_task_under_an_ended_goal_is_inconsistent`、`WorkReplay.live_goal_under_an_ended_parent_or_a_stopped_owner_is_inconsistent`、`GoalRestore.restore_state_refuses_a_change_to_a_terminal_goal_or_an_impossible_cursor`、`TaskRestore.restore_state_refuses_a_change_to_an_ended_or_held_task`、`AgentRestore.record_no_transition_writes_is_corrupt`；E2 F7（已提交区域损坏）；普通命令 effect 的局部输入边界：`CommandRecords.ordinary_effect_rejection_preserves_encoded_and_decoded_outputs`、`CommandTable.ordinary_restore_rejects_other_effects_without_changing_the_retry_window`（不替代恢复专项） |
| C-STORE-010 | 当前开发版本不提供迁移；旧 schema 在任何新写入前受限，原目录保留，不自动清空或转换；回退使用原二进制与其原数据 | 原地改写旧日志导致新旧都不完整，或旧固件忽略新状态回到旧快照 | M1 部分实现（ADR-0038 开发版不提供迁移，旧格式无新写地受限并保留）：`StorageSchemaBoundary.unsupported_schema_never_installs_a_writer_or_mutates_source`、`StorageSchemaBoundary.unsupported_schema_is_still_rejected_after_a_process_restart`、`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`；E4 步骤 6；C-VER-021 |
| C-STORE-011 | 日志轮转、双槽快照、临时输出、历史大小与写频率均有上限；维护与限频暂时冻结新 transition 并报告背压，动作屏障不能为节省写入而跳过；实际空间不足时新副作用返回 `AYNI_STORAGE_FAILED`，已映射命令仍返回首次结果。保留当前可查询对象、步骤/检查点和既有幂等窗口，不额外归档已覆盖历史 | 为减少磨损跳过屏障，或轮转删除尚未激活快照覆盖的日志，或存储满使已接受命令失去原结果 | M1 部分实现（当前工作模型的轮转、保留、空间准入和写频；动作屏障尚未实现）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`StorageRecovery.retry_wait_is_readable_and_preserves_command_lookup_before_busy`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`；真实空间不足的准入 `StorageAdmission.full_medium_refuses_new_state_but_keeps_the_original_command_result`、`StorageAdmission.an_unmapped_noncommand_also_requires_space_before_applying`；日志上限与两项预留 `StorageBudget.default_log_limit_is_eight_buffers_or_one_mib`、`StorageBudget.smallest_log_limit_holds_the_reserve_two_batches_and_an_activation`、`StorageBudget.admission_needs_the_larger_reserve_free`、`StorageBudget.a_space_shortage_holds_transitions_until_a_refresh_finds_room`、`StorageBudget.a_snapshot_needs_its_reserve_and_a_shortage_holds_transitions`、`StorageBudget.compaction_is_due_at_three_quarters_of_the_limit`、`StorageBudget.compaction_is_due_before_the_log_reserve_would_cross_the_limit`、`StorageAdmission.a_log_limit_without_room_to_compact_is_refused`、`StorageAdmission.a_pending_compaction_holds_new_work_until_the_journal_drains`；持续限频 `SnapshotFaults.every_mutating_phase_shares_the_interval_and_waits_without_spinning`、`SnapshotFaults.a_slow_strategy_cannot_spend_the_next_storage_interval_before_submit`；动作屏障随 MA-008/MA-018/MA-022；E2 末段；C-COMMIT-015 |
| C-STORE-012 | 日志扇区擦除在没有活动执行器会话时预先完成；会话期间只做页编程，以免擦除挂起停止监督（C-STOP-005） | 会话中日志写满一个扇区，触发擦除，L2 心跳超时误停 | `MA-026` 故障矩阵 |
| C-STORE-013 | 参与 transition 的状态只写入日志；NVS 只保存不参与 transition 的小型配置。必须跨介质原子更新时，以日志中的协调记录实现（C-COMMIT-014） | Task 调用计数写在 NVS、Task 写在日志，掉电后只更新了一边 | M1 已实现（工作模型：实例、Goal、Task、步骤、私有检查点与命令映射只写入日志，每个 transition 一个批次，ADR-0026）：`WorkRecovery.power_loss_takes_the_batches_never_appended`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；Task 调用计数等其余状态随各自的包 |
| C-STORE-014 | 持久记录及快照不包含单调时间值、句柄、指针、资源占用或原始媒体（C-PRIM-001、C-TIME-003）；快照只保存持久领域视图，不把本次目录缺失造成的恢复 fault/hold 永久化 | 快照保存重启前单调时刻，或换回兼容策略后 Task 因上次恢复的临时 hold 仍不能继续 | M1 已实现（现有工作记录和快照：Goal 只写时长，Task 不写 `due_us`，恢复后重新计时）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`WorkSnapshot.bytes_are_independent_of_this_boots_monotonic_clock`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`；Grant 不含 deadline/易失 hold：`GrantRecords.duration_is_persistent_while_deadline_hold_and_transition_stamps_are_not`（ADR-0030）；评审 |
| C-STORE-015 | Grant创建与完整授权命令mapping/结果同批；保留source_domain、展开scope、父链、原始stamp和精确replacement，journal/snapshot均不写deadline/易失hold；unknown/missing REQUIRED的授权记录含covered历史均受限 | 孤立Grant被恢复为权限；快照遗漏原始域或把当前hold永久化；旧reader跳过授权新记录 | M1 已实现（Grant 创建/期限及完整命令的 journal/snapshot）：`GrantApi.interrupted_append_recovers_both_create_and_exact_mapping_or_neither`、`GrantReplay.neither_a_create_nor_an_exact_mapping_may_replay_without_its_partner`、`GrantReplay.new_authority_records_require_the_flag_even_for_unknown_future_versions`、`GrantReplay.snapshot_orders_a_reused_child_slot_after_its_parent_and_rejects_the_reverse`；ADR-0030 |
| C-STORE-016 | 显式根撤销与完整当前命令结果同批；有界后代状态与根进度同批，fixed through/单调after/真实父链及独立cleanup_stamp均可重放检查，精确replacement须为持久安全叶节点 | 根已撤销但丢命令结果；后代已改而cursor未改；cursor改变终态回收序 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.root_record_and_exact_mapping_require_each_other_in_one_batch`、`GrantRevokeReplay.descendant_state_and_progress_are_an_atomic_pair`、`GrantRevokeReplay.a_later_cursor_cannot_repair_a_child_state_paired_with_another_roots_progress`、`GrantRevokeReplay.descendant_and_progress_must_name_the_same_root_and_cursor`、`RevokeApi.only_a_durable_terminal_leaf_can_make_room_after_revocation_cleanup`；最终交付见 ADR-0031；C-COMMIT-005 |
| C-STORE-017 | snapshot只读写完整Grant13/v2与mapping9/v2，普通/专属用显式绑定区分；共享classifier区分journal/snapshot及covered历史，分别检查created/last/cleanup界和effect/retained target一致性，不保存Gate/pending/pin/deadline | snapshot13/v2被journal按v1解释；NO_CHANGE与同seq CHANGED互换；cleanup超过边界仍导入 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.snapshots_require_current_complete_grant_history`、`GrantRevokeReplay.record_versions_are_specific_to_journal_and_snapshot_contexts`、`GrantRevokeReplay.snapshot_cleanup_stamp_has_independent_transition_and_commit_bounds`、`GrantRevokeReplay.snapshot_revoke_effect_must_agree_with_the_retained_target_history`、`GrantRevokeReplay.snapshot_cleanup_watermarks_cannot_name_a_future_boot`；最终交付见 ADR-0031；C-STORE-014 |
| C-STORE-018 | GOAL 当前正文的 authority_required 标记与 type17/v1 必须配对，journal 同批且 stamp 相同、snapshot 相邻且在 Goal/快照边界内；原需求不可改写；活引用必须匹配已有 Grant 的主体、原域与范围 | 缺元数据的新 Goal 被当成旧零需求；跨 Goal 借记录；按当前目录重释旧范围 | M1 部分实现（内部事实、公开创建及 journal/snapshot/boot 集成，批准随 E3）：`GoalAuthorityPersistence.journal_round_trip_preserves_zero_and_rejects_missing_evidence`、`GoalAuthorityPersistence.missing_duplicate_or_wrong_batch_metadata_restricts_boot`、`GoalAuthorityPersistence.paged_snapshot_keeps_adjacent_metadata_and_original_stamps`、`GoalAuthorityPersistence.restored_updates_keep_the_original_ceiling_and_never_return_to_proposed`、`GoalAuthorityPersistence.a_grant_for_another_agent_cannot_authorize_the_restored_goal`、`GoalAuthorityPersistence.a_narrow_grant_does_not_cover_a_wider_saved_requirement`、`GoalAuthorityPersistence.proposed_snapshots_require_the_new_state_envelope`；ADR-0037 |
| C-STORE-019 | Goal 专属/普通 Grant 在当前完整正文显式保存绑定（无绑定为0）；journal v1/snapshot v2，根/不可委派及身份规则在 struct/bytes 共用校验 | 旧 reader 丢绑定；新 envelope 接受零绑定；普通命令替代批准证明 | M1 已实现（专属记录、版本边界和整批批准恢复）：`GoalGrantRecords.complete_current_body_round_trips_bound_and_unbound_grants`、`GoalGrantRecords.invalid_bound_facts_never_publish_partial_decoded_outputs`、`GrantRevokeReplay.record_versions_are_specific_to_journal_and_snapshot_contexts`、`GoalApprovalPersistence.public_approval_survives_journal_and_snapshot_with_exact_retry`、`GoalApprovalPersistence.missing_partner_or_mismatched_exact_command_restricts_recovery`、`GoalApprovalPersistence.snapshot_command_echo_cannot_disagree_with_retained_creation`、`GoalApprovalPersistence.unrelated_work_before_during_or_after_approval_restricts_boot`、`GoalApprovalPersistence.old_dependency_must_have_been_active_at_approval`、`GoalApprovalPersistence.one_new_grant_cannot_fill_two_equal_original_missing_items`、`GoalApprovalPersistence.maximum_missing_count_with_large_domains_fits_batch_reservation`、`WorkAuthorityResult.command_record_round_trip_and_all_truncations_are_atomic`、`WorkAuthorityResult.current_generation_result_keeps_its_establishment_stamp`、`GoalAuthorityPersistence.ordinary_metadata_cannot_clear_a_persisted_deny`、`GoalAuthorityPersistence.proposed_authority_cannot_precede_immutable_creation`、`GoalAuthorityPersistence.current_goal_sequence_cannot_exceed_snapshot_coverage`、`GoalApprovalPersistence.retained_result_cannot_substitute_another_covering_old_grant`、`WorkAuthorityResult.enclosing_mapping_sequence_must_match_the_saved_binding`、`WorkAuthorityResult.shared_creation_echo_keeps_distinct_nonzero_delay_and_period`；ADR-0037；补充验证：`WorkCommandCodec.approval_checks_each_shape_and_atomic_output_boundary` |

## Goal 授权元数据（MA-006 E/F）

type17/v1 为完整当前授权元数据。GOAL.authority_required 声明同批/快照相邻伙伴，GOAL_STATE.change 明确授权、暂停或重新授权证明；缺失、孤立、重复或跨批伙伴损坏。公开创建必须有显式元数据，包括 count=0；每 transition 至多一条。

全部整数为小端。固定头 72 B：Goal ID（u64）、最后 authority transition_id（u64）、commit_seq（u64）、phase（u32，1=proposed、2=authorized、3=paused）、count（u32，0～16），再接五个 u64：binding_commit_seq（offset 32）、parent_goal_id（40）、parent_binding_commit_seq（48）、input_incarnation（56）、input_through（64）。随后按需求顺序重复：Capability version（u32），Capability ID、原域、Scope（三段各带 u32 长度的字节串），Grant ID（u64）、选定 revision（u32），business_contract 长度串。每项固定开销 32 B，不包括四段数据；总长必须恰好匹配。

Goal ID 必须是有效 Kernel ID，stamp 两项非零；Capability ID/版本与原域必须有效，Scope 完整且被原域包含。Grant 引用或是 `{0,0}`（缺项），或是有效 Kernel ID 与非零 revision。proposed 至少缺一项且 binding 全零；authorized/paused 全部引用完整，binding_commit_seq 非零且不晚于 authority commit_seq。父 Goal ID/绑定序号同零或同非零；根为零，子项须与准入时父绑定相等。输入 incarnation=0 时 through 必须为 0；非零 incarnation 可配零水位（无输入），不得超过 boot ID 范围。输入恢复边界由完整重新授权事务证明。显式 count=0 可授权或暂停，缺少整条记录仍是未知元数据，不能当零需求。表的配置容量可以小于记录绝对上限，放入时不足则不改变已有事实。

journal/snapshot 使用同一 codec。journal 正文 stamp 等于 batch，Goal 状态必须在同批更新；已有元数据的原需求不可改变，stamp 严格前进，授权/暂停不能退回 proposed。snapshot stamp 在覆盖边界内且不晚于 Goal 的最后提交。Grant 选择保存 ACTIVE revision，与后来终态 revision 不同不构成损坏；所选主体、原 schema/domain 与覆盖关系仍须一致。活 Goal 所引用 Grant 缺失损坏，持久终态 Goal 可保留已回收历史引用。新 boot 的目录兼容与可消费性单独检查，不改写原需求。记录不保存单调时间、指针、进程句柄、输入队列状态或 Grant 的易失引用释放 fence。

PROPOSED 必须有至少一项缺引用，taken/waiting/next_scheduled/last_task_id 为空且无子 Goal/Task；授权 paused 与普通 ACTIVE/PAUSED 独立，不能由普通状态/元数据解除。新的绑定须由完整重新授权事务建立。

## Task 授权元数据（MA-006 F）

ADR-0038 决策 2～6：type 18/v1 已接入 arena、journal、snapshot、发现及容量预算，必须 REQUIRED。journal 每批至多派生一个新 Task，按 TASK→TASK_STATE→TASK_AUTHORITY 配对，状态后必须紧接该 Task 的准入，批次结束不能留未配对身份；snapshot 在该 Task 的步骤/检查点/状态后紧接同一准入，可跨页。整批或整份快照拒绝缺项、重复、错 Task/Goal/绑定/引用/stamp、未来 boot 身份及夹带记录。旧 schema 在新写入前拒绝。

全部小端，原字段头 48 B：offset 0 task_id u64、8 goal_id u64、16 goal_binding_seq u64、24 transition_id u64、32 commit_seq u64、40 phase u32（1=authorized、2=paused）、44 count u32。随后 count 项，每项 Grant ID u64/revision u32，共 12 B，顺序对应原 Goal 需求。count=0 是明确零需求，不是缺少事实。随后 tool_count u32 及每项 source_call_id u64、selected Grant ID u64/revision u32（20 B），来源按 Call ID 严格递增；固定开销共 52 B。源 Call 保存初选 Grant；目录数量不计入必需 count。

Task/Goal 为不同的有效 Kernel ID；binding sequence 正且不晚于记录 commit_seq，transition_id 正；count≤16，每个引用为有效非零 ID/revision。reader 与 struct 共用语义验证；截断、尾字节、非法字段不改输出。表按稳定 Task.slot 和确切 task_id 放置，配置不足不改任何行。相同绑定只能保留同一引用，不能 paused→authorized；新绑定须单调，Task/Goal/需求数不变，stamp 两项严格推进。F2 在消费和重放复用同一 Task/Goal 精确匹配，活及终态 Task 都须保存相同 Goal 绑定、需求数及引用；Scope/父链检查沿用 Goal/Grant 属主。重新授权配对随 F4。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-020 | Task 保存独立完整准入引用，按稳定 slot/exact Task ID，显式零需求与缺席不同；struct/bytes 共用校验，错误不发布部分事实 | Task 位置移动或复用后借用另一 Task 权限；截断发布前缀 | M1 已实现（准入事实、持久配对、查询及显式重新授权）：`TaskAuthority.explicit_empty_admission_and_slot_reuse_preserve_identity`、`TaskAuthority.copied_references_and_capacity_failures_preserve_the_arena`、`TaskAuthority.same_binding_can_pause_but_cannot_resume_or_change_references`、`TaskAuthorityCodec.empty_and_maximum_admissions_round_trip_without_scope_copies`、`TaskAuthorityCodec.every_truncation_trailing_bytes_and_malformed_fact_preserve_output`、`TaskAuthorityPersistence.terminal_snapshot_rejects_missing_or_substituted_admission`、`WorkReplay.task_replacement_cannot_hide_a_pending_admission`；显式重新授权由 `GoalReauthorization.replaces_goal_and_ready_task_then_retries_without_reapplication` 覆盖；ADR-0038；补充验证：`WorkAuthoritySnapshot.every_retained_record_refuses_truncation_unknown_flags_and_versions`、`WorkAuthoritySnapshot.task_partners_refuse_individually_framed_identity_and_progress_mismatches` |


## 工作授权清理事实（MA-006 F3）

ADR-0038 决策 8：type 19/v1 固定 44 B，全部小端：transition_id u64（offset 0）、commit_seq u64（8）、phase u32（16，0=idle、1=scanning）、after_goal_id u64（20）、through_goal_id u64（28）、requested_through u64（36）。work 拥有状态和共享 reader/writer；Kernel journal、snapshot、发现及容量预算均接入 REQUIRED 记录，不从缺席推断清理完成。快照在 Task 区后、命令区前必须恰好保存一项，包括空表的明确 idle；开发期不为缺项旧快照补事实。

idle 的三个游标全部为零；初始 stamp 两项均零，使用后两项均正。scanning 的 stamp 为正，through 是固定有效 Goal ID，after 是零或小于 through 的有效已访 ID。requested_through 为零或下一轮的有效 Goal ID，即使等于当前 through 也须保留，不能因本轮结束丢失。空范围请求仅更新见证 stamp。稳定查询合并保留 Goal 和专属 Grant 的 Goal ID，包含 Goal 槽位已复用的旧身份；每步恰访最小未访 ID，或证明范围为空后结束，并保留下一轮请求。进度消耗 step item/time 预算，背压/快照冻结时暂停推进，即时消费检查继续拒绝失效权限。

授权暂停使用 GOAL_STATE 4/v1 的 change=4 紧接 GOAL_AUTHORITY 17/v1，只允许既有活 Goal 从 authorized 进入 paused；用户 lifecycle/revision、失效标记和触发游标逐字段保持。直属活 Task 使用 TASK_STATE 6/v1 的 change=2紧接 TASK_AUTHORITY 18/v1，原用户/策略状态、revision、步骤和检查点保持，终态 Task 不得被更新。整批末尾的 type19 证明全部直属活 Task 已暂停，不能跨批补齐、覆盖待配对身份或在 footer 后夹带记录。目录兼容和主体不可用仍是独立 hold，不因后台扫描变成持久授权暂停。 journal 最终一致性与 snapshot 共用校验：暂停 Goal 的直属活 Task 必须授权暂停；健康 Goal 可以有工具授权独立暂停的 Task（ADR-0085）；不把祖先尚待扫描的 phase 套给子 Goal，也不改写终态 Task 历史。旧暂停 journal 缺少独立 Task deny 时在新写入前拒绝并保留，不以用户 PAUSED 字段推断或补授权事实。

有 ACTIVE 专属 Grant 的 Goal 进入终态时使用 GOAL_STATE 4/v1 的 change=3，同批请求清理。扫描写完整 GRANT_STATE 14/v1，lifecycle 为 GOAL_ENDED（4），只结束本步选中 Goal 的全部 ACTIVE 专属根，不改写既有终态顺序或伪造管理撤销；该 Goal 必须已于更早 transition 终结或已让出槽位。删除一项/全部结束记录、替换成另一 Goal 或仅推进游标均不构成完整一步。到期记录 14/v1 和根撤销 15/v1 要求同批 type19 请求；无旧版本配对分支。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-021 | 工作授权清理固定 through、单调 after，扫描中新请求保留给下一轮；记录与所属状态同批且有界推进，不丢清理请求 | 新请求扩大当前扫描导致不收敛；结束时丢掉同 frontier 的请求；槽位复用混入新 Goal | `WorkAuthorityCleanup.fixed_frontier_retains_requests_for_a_second_pass`、`WorkAuthorityCleanup.interrupted_scan_keeps_its_frontier_and_later_request`、`WorkAuthorityCleanup.dedicated_grant_ends_and_reclaims_after_goal_slot_reuse`、`WorkAuthorityPersistence.ended_grant_cohort_and_footer_are_one_complete_scan_step`、`WorkAuthorityPersistence.pause_keeps_user_state_and_requires_all_direct_task_facts`；ADR-0038 |


## 原子重新授权事实（MA-006 F4）

journal 使用 REQUIRED `GOAL_STATE` 4/v1（change=5）、`GOAL_AUTHORITY` 17/v1、每个直属活 Task 的 `TASK_STATE` 6/v1（change=3） 紧接 `TASK_AUTHORITY` 18/v1、`COMMAND_MAPPING` 9/v1，最后为请求清理的 19/v1。Goal 状态保留原用户/触发状态并严格增加一次 revision，只可按触发规则跳过积压；Task 状态保留 lifecycle、hold、步骤数和 checkpoint，严格增加一次 revision；元数据原 domain/Scope/count 不变，建立新代次并同时替换有序引用。一个 transition 只改指定 Goal 的完整直属活 cohort，其他记录不得前后夹带；command 完整回显、原双 CAS 与保存的结果必须精确配对。footer 只允许请求下一轮扫描，不能借此推进已有游标。

重新授权 canonical 固定 84 B 加两个 CatalogId 正文和参数：kind/revision u32、Goal ID/授权事实 CAS u64，再接批准共用的 60 B 原创建 echo；最大 148+goal_parameter_max_bytes，不超过批准输入预算。当前命令正文保存本次不可变结果，input incarnation 为正；新引用必须来自此前已持久的 Grant，禁止批准专属根的同批发放例外。Goal 结束后的专属 Grant、父绑定和旧结果各按其事实属主验证，不能用当前引用反推历史命令。journal 的 change 保护配对证明；snapshot 使用当前完整事实与独立历史命令结果，state change=1。

重放每条 Goal 授权事实时，从 BootSequence 借用该记录之前的已持久 boot 上界，要求 input incarnation 不超过该上界；自动重载可保留更早 ingress 实例，不要求等于当前 boot。后来的有效换绑、Goal 回收或 BOOT_EPOCH 不得追认历史中的未来输入实例。活 Task 必须精确匹配当前自身 Goal 的绑定；phase 保持原必需暂停规则，同时允许健康 Goal 下的按需工具 Task 独立授权暂停（ADR-0085）；终态 Task 可保留旧代的合法历史引用，已安全回收的 Grant 不补造记录。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-022 | 重新授权的 Goal、全直属活 Task、原始双 CAS/回显、不可变结果与请求 footer 构成一个封闭批次；各前缀/重复/错配均拒绝，每条历史恢复边界受当时 boot 限制，快照保留新旧结果而不重建历史 | 合法 CRC 删一项 Task 或命令仍恢复；把清理步夹进请求；后续正常 R2 掩盖 R1 未来输入实例；同批新发 Grant 冒充已有授权 | M1 已实现（本地 C ABI 与持久场景）：`GoalReauthorizationPersistence.crc_valid_partner_faults_reject_the_whole_batch`、`GoalReauthorizationPersistence.every_complete_record_prefix_requires_all_partners`、`GoalReauthorizationPersistence.paged_snapshot_keeps_old_approval_and_new_binding`、`GoalReauthorizationPersistence.historical_result_stamp_boot_and_revision_are_bounded`、`GoalReauthorizationPersistence.later_binding_cannot_hide_a_future_journal_input_incarnation`、`GoalReauthorizationPersistence.replacement_cannot_issue_a_same_batch_dedicated_root`、`GoalReauthorization.interrupted_append_recovers_all_or_old_admissions`、`GoalReauthorization.failed_or_unknown_append_preserves_atomic_recovery_and_retry`、`GoalReauthorizationPersistence.later_task_cancellation_cannot_hide_an_omitted_live_cohort`、`GoalReauthorizationPersistence.replacement_cannot_resume_a_user_paused_goal_in_its_state`、`GoalReauthorizationPersistence.ordinary_task_state_cannot_follow_the_exact_command`、`GoalReauthorizationPersistence.management_footer_cannot_also_consume_a_cleanup_step`；ADR-0038；补充验证：`WorkCommandCodec.reauthorization_checks_each_shape_and_atomic_output_boundary`、`WorkAuthorityHistory.every_original_echo_field_is_checked_for_both_command_kinds` |

## Task 运行限制（ADR-0043）

每条完整 TASK_STATE 均保存 used/limit/blocked；used<=limit，1<=limit<=64，blocked 只在请求超限时置1且要求 used==limit。提高上限的 change=4 与 exact command 配对，snapshot change=1。暂停/取消/授权改变保留其他运行事实。

提高模型上限 canonical 为 kind13/expected_revision（u32）、task_id u64、new_limit u32，共20 B；只提高上限、清除此项暂停、revision+1，其他字段不变。批次只含状态与 exact command，无额外记录。

## 统一调用记录（ABI 0.31，ADR-0045）

schema10 的 REQUIRED `CALL_STATE` type21/v2为首次接受的完整行与完整快照，固定448 B；type21/v3仅在journal更新已有Call，固定264 B。固定长度均不含变长正文、ID和资源ID。整数小端、变长串带u32长度，读写共用mutable字段walk（ADR-0104）。

完整v2顺序：

1. call/operation/task/owner四个u64；Binding/Capability/Provider version与mode四个u32；step、Binding、Capability、Provider、endpoint、destination六个长度串。
2. 原CallContract的13个u32及各资源ID长度串/units u32；原authority domain及规范CallInput两个长度串。
3. **可变部分**：attachment u32；created/latest stamp各boot/transition/commit三u64；attempt两u64；progress_sequence u64；cancel_cause/cancel_result/outcome/status/reason五u32；query固定60 B（generation/phase、requested stamp、attempt、status/reason/conclusion）；model usage固定32 B；结果引用id/digest u64、size/tool_count u32，共24 B。
4. 结果、嵌入ActionState两个长度串；has_execution/content_erased两u32；erasure stamp三u64；result_kind u32；可选ExecutionFact及subscription尾。全部标记仍只有0/1；ActionState复用20/v1 codec。
5. 原business_contract长度串；原Grant id u64/revision u32；model context boot u64、visible四u64、sample_count u32及其活动样本前缀。

增量v3顺序仅为原call_id u64和上述第3/4项可变部分，不能创建Call、替换身份、契约、原域或实际参数。重放先找到原行，再用同一共享validator及原Task/事务配对校验；首次完整行不能作为后续替换。snapshot只接受完整v2，按稳定Call ID输出。PREPARED和DISPATCHING仍各占有序transition，可共享一次Host持久确认，最终claim须等DISPATCHING前缀。

结果引用为零时其余引用字段全零，结果内联。非零时结果内联为空，必须COMPLETED且未擦除；MODEL保持TEXT/TOOL_CALLS语义。明确启用SHARED_RESULTS的部署可让Core把自身已验证、规范化的普通TEXT/VALUE结果放入正文存储；VALUE的原Schema保留，读取时按result_kind取规范值字节。Provider不能借此直接提交未经验证的工具正文。Core不再在每次状态日志重复结果原文；聊天取得独立引用，以原proof/domain重建展示，Call退休不删除聊天仍持有的正文。

完整TASK_STATE仍保存当前call/end。受管接受与步骤同批，后台完成不结束另一前台；未知结果、未释放借用和未停止执行继续阻止不安全回收。查询与恢复等待的精确命令格式和原事务伙伴不变。schema10没有旧reader或迁移器，格式不匹配保留并拒绝，不能自动清空。新格式异常恢复未验证，正常codec/关闭重开不能替代此项证据。

### 按需工具来源（自 schema 6，当前 schema 10 / ADR-0085）

GOAL 尾部工具策略 mode 为 0=REQUIRED、1=ON_DEMAND；count 0..256，各项依次为 Binding version u32、role u32（0=capability、1=shell，至多一项）、Binding ID 长度串，ID 不重复。预算和恢复容量上限同时受部署 Binding capacity 约束；策略按原顺序持久化，不回查新 GoalType 填充。

TASK_STATE.change=6 表示 tool_bind：原必需引用、旧工具前缀和授权 phase 保持，只追加一项。Task 状态与 TASK_AUTHORITY 后，须在同批出现该 source Call 的首次接受，Task/source ID/原 Grant/当前选择精确配对；不能跨批补齐。change=7 表示 tool_pause：不改变任何引用或用户状态，authorized→paused，而 Goal 可保持健康。显式重新授权仍使用 change=3，原子更新应用时 direct-live cohort，只替换 selected，不替换 source；Goal CAS 不是工具集合快照 CAS。

最终一致性双向核对 Call 与 Task 引用；活或尚不可安全退役的 Task 必须保留原 Grant 与 selected Grant。原 Grant 按 source Call 创建时刻证明，selected 按 max(source created seq, Task Goal binding seq) 证明，不能以当前 Catalog/ACTIVE 状态判断历史合法性。正文已擦除时复用 privacy/provenance 证明，不重建参数。新路径的故障/恢复专项未执行，既有专项回归继续保留。

## 调用扩展与工具结果

Action 长度串后始终编码 execution_present、erased_present（u32）、content_erased ActionStamp（24 B，缺席全0）、result kind u32。result kind 1=UTF8、2=TOOL_CALLS、3=VALUE、4=UNAVAILABLE；最后一种表示正文失效但身份/结果/效果事实保留，不能作为可消费结果，独立于 content_erased；实际列表只允许完成结果。随后按 presence 写 ExecutionFact；contract form 为 subscription 时再写订阅事实；最后始终写 business_contract 长度串，再写原可选授权 Grant ID u64/revision u32（必需调用同零；按需 TOOL 初次接受为确切 ACTIVE 引用），再写 ModelContext 固定44 B：boot_epoch u64、4 个 visible u64（256 位按原 Goal 策略顺序标记）及 sample_count u32；随后每项 sample 为 source Call ID/generation/sequence 三个 u64（24 B），最多8项。原 Call 拥有 source/version/Grant，跨行验证同 Task 且源先于派生调用；运行时按原 ObservationRef 复核 TTL/gap/关闭/代际。无来源时全零；模型提议继承原模型目录披露来源，实际业务 Call 不继承。派发与读取均复核，旧启动来源不能作为当前可消费正文。跳过、物理、工具、擦除、订阅是当前领域变体，共用 v1，不再选择布局版本。

ExecutionFact 原固定112 B及各 ID、数值投影仍按属主顺序编码；每个观察引用固定40 B加 source ID，最后的 subscription_id u64 始终存在，0=Host。phase/target/stop、四个 epoch、calibration 和原约束不可倒退；不保存 nonce、deadline、在线占用或 raw media。擦除不能改写已知物理执行结论，UNKNOWN 不自动重试。

工具列表 format3：magic u32=0x33435441（ATC3）、version u32=3、count u32（1..8）、assistant text 长度串。各项依次为 function_name 长度串、input kind u32（2=ARGUMENTS、3=INVALID_ARGUMENTS）、opaque protocol ID 长度串、payload 长度串。每项固定开销 16 B，协议 ID 为 1..128 B UTF-8，列表内唯一，与 Ayni operation/attempt 身份分离。ARGUMENTS 保留原数值 lexeme，INVALID_ARGUMENTS 保留完整内层非法 JSON UTF-8 原文；业务规范化留给后续 Call。总大小由原 result limit 及 65536 B 全局上限限制，拒绝 ATC1/2 和尾字节；不保存外层模型 JSON。

### 业务值、具名源与原契约证明（ABI 0.48 / ADR-0084）

下列 codec 均由 `catalog/capability` 拥有，不使用 ABI padding、指针、宿主句柄或 C++ 内存布局。`source_domain` 保持独立的授权域格式；业务证明不是拼入原域的隐藏后缀。启用 Call 的 Capability、Provider offer、Grant、Goal requirement 及 Call 各自保存独立完整证明，关闭目录时精确比对，不能以本次启动的不同 Schema 重释历史事实。纯授权能力没有 Call 契约时证明为空。

- **VALUE v1**：version/count（u32，头 8 B），随后 preorder 节点各为 kind/field_id/subtree_end（u32，头 12 B）加活动 payload。OBJECT/LIST/NULL 无 payload；BOOL u32；I64/U64/binary64 各 u64；DECIMAL 为 coefficient u64 位模式和 scale u32；TEXT/ENUM/IDENTIFIER 为长度串。规范对象按稳定 field_id 升序、默认值已补齐、列表保留顺序/重复、浮点 -0 规范为 +0；无尾字节。业务 input_limit 计整个编码，不能只计算 text 长度。
- **ARGUMENTS v1**：version/count u32（8 B），每节点 kind/subtree_end u32、name 长度串，加 BOOL u32 或 NUMBER/STRING 长度串；容器/NULL 无活动 payload。NUMBER 保存原始数值 lexeme，未经过 double。Core 对原 Schema 解析名字与类型；重复/未知键、错误数值表示、越界或容量不足整次拒绝。
- **Schema v1**：version/count u32，每节点 kind/field_id/规范 subtree_end/flags u32，name/unit 长度串，随后仅该 kind 的边界：I64/U64/binary64 为两个 u64，DECIMAL 再接 scale u32，TEXT/LIST 为 min/max u32，ENUM/IDENTIFIER 为 count u32 和规范排序 choices 长度串，OBJECT/BOOL 无边界。每节点最后为 default VALUE 长度串，0 长表示无默认。对象字段按 ID、choices 按字节排序；每 Schema 最多 32768 B、128 节点，总 choices/default 节点各最多 256。
- **business_contract v1**：version u32、完整 execution contract（上述 13 个 u32 及资源），subscription presence u32（有时为 source 长度串、source_version/lease_ms u32），input/result 两个 Schema 长度串，projection count u32。每 projection 为 field_id/flags/scale/path_count u32、unit 长度串及 path_count 个 u32；按 authority field_id 升序。最大 69632 B，配置默认 4096 B，独立计入各表、暂存、journal/snapshot 预算。

最大深度 8、值节点 256、单列表 64；decimal coefficient 为 I64、scale 0..18。只有声明 nullable 才接受 NULL。投影只能沿声明路径，至多一个共同非空列表轴；浮点/自由文本不参与权限比较，BOOL 只能显式映射 0/1，decimal 的 scale/单位不可隐式转换。全部列表行由同一完整 requirement/Grant 覆盖，先前不完整候选不阻止尝试后一个完整候选，不把多条授权拼接。

schema 6 在新增写入前拒绝旧 schema 0..5 并保留源目录，无旧 reader/迁移器；回退使用旧二进制及原目录。此规则不是新故障/恢复专项已验证的声明。

Scene type22/v1 完整正文不变：44 B固定部分加原声明。启停与 exact scene command 同批且不混入其他变更；保存原声明，不在恢复时查当前 catalog。Scene mode、条件/规则及物理投影由原属主校验。

## P3 记忆与内容来源记录（ADR-0060，ABI 0.40）

以下当前记录全部 REQUIRED；未知类型/版本均拒绝并保留原目录。除注明快照专用外，journal/snapshot 共用属主编解码；整数小端，无 native padding。

**共享来源列表**：`count:u32` 后接 count 个 52 字节引用，顺序为 `kind:u32, id:u64, revision:u32, actor_instance_id:u64, purpose:u32, read_grant_id:u64, read_revision:u32, egress_grant_id:u64, egress_revision:u32`。kind 为 memory=1/context=2；无 Grant 使用 ID/revision 同为 0。格式上限 8；当前配置更小归为容量不足，非法引用仍归损坏。完整闭包包含实际权限，不允许同一身份的不同版本替换合并。

| 类型/版本 | 记录体与配对 |
|---|---|
| 23/v1 MEMORY | 32 字节头：`id:u64, revision:u32, state:u32, transition_id:u64, commit_seq:u64`；后接 MemoryValue：`namespace:u32, namespace_id:u64, source:u32, profile_field:u32, confirmed:u32, source_call_id:u64`，再接长度前缀 body、tag_count 和各 tag、keyword_count 和各 keyword、共享来源列表。值固定开销 48 字节，包含四个长度/count 字段。管理变更与 exact 命令同批；策略写与 Task checkpoint/provenance 同批。删除/修改另需同批 privacy 请求。 |
| 24/v1 PROVENANCE | 32 字节头：`work_id:u64, kind:u32, content:u32, transition_id:u64, commit_seq:u64`，后接共享来源列表及 origin_context_id/origin_file_generation（u64，id=0 时 generation=0；有效 Context 的 generation 可为0）。kind 为 Goal=1/Task=2；content 为 available=1/erased=2。available 必须与 Goal 创建或 Task checkpoint 的 stamp 精确配对；erased 只作为快照表示，正文/依赖均为空。 |
| 25/v1 PRIVACY | 固定 48 字节：`request_seq:u64, phase:u32, effect:u32, cursor:u64, snapshot_generation:u64, transition_id:u64, commit_seq:u64`。请求与来源失效同批；普通进度单独批次且只改变一个属主表示。 |
| 26/v1 CONTEXT | 固定 80 字节后接共享来源列表：`id:u64, owner_instance_id:u64, revision:u32, pending_action:u32, generation:u64, last_turn:u64, old_revision:u32, reserved:u32=0, turn_id:u64, task_id:u64, transition_id:u64, commit_seq:u64`。OPEN/PUBLISH/FORGET/ACK 必须有精确命令伙伴；FORGET 另需 privacy 请求。清理诱发的 Context 擦除仅由 privacy 进度记录表达。 |

Goal/Task/Checkpoint 的 provenance_required 是完整当前正文的一部分；无 footer 的待定工作不能发布。Provenance origin 始终编码，0/0=无原始 Context。命令直接保存来源列表；擦除使用 content=2，不再包装另一版本或嵌套正文。Call 的完整当前擦除标记保留 outcome、latest 及独立物理事实。

清理 phase：memories=1、contexts=2、goals=3、tasks=4、calls=5、commands=6、files=7、compact=8、purge=9、complete=10；idle=0 不序列化。effect：none=0、memory=1、context=2、goal=3、task=4、call=5、command=6、remove_memory=7。cursor 是对象稳定 ID；命令阶段使用原 commit_seq。Context effect 之后唯一允许的回退是 memories/cursor=0/effect=none，直至来源稳定；其他普通进度不能倒退。files→compact 须所有 Context ACK 且外部借用释放；目标 snapshot_generation 为当前代际+2，溢出明确停止并报告容量失败。purge/complete 快照不能宣称超过本快照 generation 的轮转已完成。scrubbed Memory 从新快照省略；erased Provenance 保留不可用身份标记，正文/依赖为空，运行时完成双槽覆盖后移除 tombstone。

**会话私有格式（ADR-0108）**独立于Core schema10，无旧reader或迁移；未知版本保留并拒绝，不自动擦除。Native的SQLite5只保存共享值；AYFV1文件、正文AYNB1及4 KiB块布局见[C-HVAL](../host/file_values.md)，Native具体SQL约束见[Native会话](../host/native_conversation.md)。

- **AYNC6**：8字节magic/version后，Conversation字段由`strategies/builtin/conversation.cpp::fields`单一读写遍历定义。文本均为24字节BodyRef(id:u64,digest:u64,size:u32,reserved:u32=0)，包括summary/draft/history/rounds；保留原id、owner、隐私/文件版本、轮次、usage、披露来源和context_version，不复制正文。rounds索引为count:u32后逐项number:u64/user:BodyRef/answer:BodyRef，user必有、answer可无；稀疏轮号不复用。
- **AYNT8**：8字节magic/version后为done:u32/base_revision:u64/memory_id:u64，再接同一Conversation字段、input:BodyRef、phase/compact_resume两u32及controller_fields。结果摘要源为完整BodyRef与json_bytes:u32，后续offset/next/chunk三u32和summary:BodyRef；不再保存Native archive结果ID。模型提议和过长结果的临时投影只属于活Turn缓存，不持久编码；冷读按原Call来源重新构建，不重执动作。
- **ANRV2结果视图**：固定152字节小端，magic=0x56524e41/format=2（偏移0/4，u32）；原logical_call_id/operation_id/requires_seq（8/16/24，u64）；outcome/status/reason/effect/result_kind/reserved（32/36/40/44/48/52，u32，reserved=0）；value/domain/proof/presentation四个BodyRef（56/80/104/128）。前3个引用必有，presentation可无；终态completed/failed/cancelled/skipped，排除未知结果/未知effect。一个ViewCodec共用读写，读取再验证所选来源的Call身份/序号、终态和完整值/域/证明内容身份；presentation只改变给模型的展示，不替代原值和证明。


## P4 日历记录（ADR-0061，ABI 0.41）

schema 6 中所有日期字段均在完整当前正文内；不认识的格式在写入或执行前拒绝并保留原目录。

- **type 27/v1 CALENDAR**：64 字节固定部分后接 TZ 字节。依次为 `goal_id:u64, transition_id:u64, commit_seq:u64, last_local_s:u64, last_utc_s:u64, taken:u32`，再接规则 `kind:u32, rule_version:u32, first_local_s:u64, timezone_length:u32, timezone_bytes`。未消费的两个 last 字段编码为 UINT64_MAX，其他时刻范围 0～253402300799 秒。TZ 长度 1～128；规则 kind 1/2/3 为单次/每日/每周，rule_version=1。日志和快照共用正文；仅规则创建或消费/跳过时更新 stamp。Goal 槽退役同时清空本行。单调投影、首次容量等待时刻和时钟可信状态均不持久化。
- **GOAL/TASK**：calendar trigger 要求对应 CALENDAR；GoalState.next_scheduled 对日历恒为0。Task 始终包含 calendar 两时间与 presence，occurrence 计数对应规则本地键；快照保存原历史，不按长度推断布局。
- **管理命令原始 Goal echo**：仅 CALENDAR_* 在原创建字段之后追加上述完整规则值（20 字节固定部分加 TZ）；普通 Goal 编码不变。审批/重新授权比较完整规则，不能仅比较日历 kind 或当前系统时区。

完整事务须保持 Calendar 游标、Goal 状态、Task 创建与初始元数据同批；周期重新授权跳过积压的 CALENDAR 伙伴限定同一 Goal 和 stamp。实际故障/恢复矩阵仍未验收，正常重开证据与最终测试结果见对应 PR。

## Task 订阅与等待（ABI 0.43，ADR-0063）

CALL_STATE 的订阅尾由 contract form 决定：source 长度串，source_version/lease_ms/phase/desired（u32），generation/lease_sequence（u64），control_status/control_reason（u32），固定44 B加 source。generation 是 Kernel ID，等于本代 action.operation；只在已知停止且旧借用归还后换代。

STARTED 的前台转后台、步骤完成、Task 前台释放同批；后台回执不要求更新 Task.current_call。完整 TASK_STATE 始终保存 wait presence 和前沿；唤醒/终态清除，其他元数据更新保留。跨表校验所属 Task、订阅、前后台外键和代际，UNKNOWN 阻止不安全回收。正常重开及本次必要恢复验证各自提供证据，不能推及未执行的专项。

## P5 暂停组记录（ADR-0065，ABI 0.44）

- REQUIRED `PAUSE_GROUP` type28/v1，正文 56 B：`group` u32、`paused` u32（0/1）、稳定 `object_id` u64、`revision` u32、reserved u32（0）、`transition_id` u64、`commit_seq` u64、逻辑输入 `incarnation` u64、`through` u64。编号 1～8；对象/版本/stamp 正，输入前沿复用 C-AUTH 的逻辑格式，不存单调时间。默认开放且未控制的组不写记录；没有新建/回收/重用组 ID 的路径。
- 日志组事实排在所有工作/日历变化后及 COMMAND_MAPPING 前，必须与本批确切所有者命令配对；快照通过 PersistentStore 完整恢复每个已控制组，之后才结算 Task。
- 当前完整 COMMAND_MAPPING 的 canonical，canonical 16 B：kind=24 u32、expected_revision u32、group u32、paused u32。result revision 是 expected+1，effect=CHANGED。相同状态的新命令也推进一次控制 revision；同命令重试只返回首次结果。
- Goal 审批/重新授权的 canonical echo 仅在非零组时末尾追加 group u32；无组值不追加。此业务请求格式与存储 schema 独立；无旧存储迁移工具。

### Binding 资源映射（schema 8 / ADR-0087）

CALL_STATE 的资源组保存接受时已解析的实际 ID 与原需求单位；原业务证明内仍是 Capability 的逻辑组。固定长度与字段顺序不变。读写共用原 codec，本征校验要求非资源语义一致、需求数/单位一一对应；派发另要求当前确切 Binding 的有效完整契约与原 Call 相同。无持久配置映射表，无旧 schema reader/迁移。用户2026-10-31授权的最小正常重开/映射不兼容验证见 C-BIND-004；其他新故障/恢复专项仍未执行，不由此推定完整通过。
