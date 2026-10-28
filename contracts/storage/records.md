# 持久记录格式

- **关联**：计划 §2.8、§4.2「持久化」、§4.3、§4.4「升级」；ADR-0002、ADR-0016、ADR-0026、ADR-0027、ADR-0030、ADR-0031；`MA-010`、`MA-006`；E2 F0–F8、F11、F12，E4 步骤 6
- **属主**：`durability`（批次编码、持久前缀、快照、重放、栅栏记录、迁移机制）；领域记录的内容由各属主模块定义，`durability` 不理解其语义
- **状态**：M1 当前批次与快照只读写 `storage_schema=2`；旧 schema 0/1 在新写入前只读受限并保留，不迁移、不自动清空或回退（ADR-0038 决策 6）。布局以本文件和编解码测试为准；Native/模拟证据不代表 ESP32 或真实 OTA 验证；C 的 Grant/完整命令扩展及 D 的撤销扩展各自验证与交付状态见 ADR-0030/0031；D 已通过 Native 场景、覆盖率与内存量测，#28 已合并

## 编码约定

- 全部整数为定长小端序；字节串以 `uint32` 长度前缀；不持久化 C++ 内存布局（C-VER-023）。
- 校验和为 CRC-32/ISO-HDLC（与 ESP32 ROM 的 `esp_rom_crc32_le` 相同，Native 由 `foundation` 实现同一算法）。选它是因为 ESP32 可直接用 ROM 实现，不增加固件体积；CRC 只用于发现损坏，不提供授权或防篡改（C-STORE-008）。

## 日志批次

一个提交批次是一个 transition 的全部记录（`domain/commit_and_output.md`）。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYJB` |
| 4 | 2 | `storage_schema` | 存储格式版本 |
| 6 | 2 | `header_length` | 本头部长度（当前 44），含末尾的 `header_crc`；追加的头部字段插在 `header_crc` 之前 |
| 8 | 4 | `writer_epoch` | 写入方代际（C-STORE-005） |
| 12 | 4 | `flags` | 保留，写 0；读到未知位时进入 `RESTRICTED` |
| 16 | 8 | `commit_seq` | 连续递增 |
| 24 | 8 | `transition_id` | 重试同一 transition 时不变（C-COMMIT-008） |
| 32 | 4 | `record_count` | 记录数 |
| 36 | 4 | `payload_length` | 记录区字节数 |
| 40 | 4 | `header_crc` | 始终是头部最后 4 字节（偏移 `header_length − 4`），覆盖偏移 0 至 `header_length − 5`，追加字段因此同样受保护 |
| 44 | `payload_length` | 记录区 | 见下表 |
| — | 4 | `commit_magic` | ASCII `AYJC` |
| — | 4 | `commit_seq_low` | `commit_seq` 的低 32 位，把提交标记绑定到本批次 |
| — | 4 | `payload_crc` | 覆盖记录区 |

Host 通过 `include/ayni/storage_format.h` 的 `ayni_storage_read_batch_identity`
提取前 32 字节的身份（ABI 0.28，ADR-0040）；该纯函数不校验 schema、CRC、
marker 或身份语义，不能替代 Core 的完整批次校验。失败不修改输出。

每条记录：

| 长度 | 字段 | 说明 |
|---:|---|---|
| 2 | `record_type` | 见“记录类型” |
| 2 | `record_version` | 该类型的版本 |
| 4 | `record_flags` | 位 0 = `REQUIRED`：读不懂时必须进入 `RESTRICTED`；其余位保留，写 0，读到未知位时进入 `RESTRICTED` |
| 4 | `length` | 记录体字节数 |
| `length` | 记录体 | 由属主模块定义的字段编码 |

## 记录类型

名称、所属批次与属主在此固定；类型编码在实现该记录的工作包中分配，记入下表。

| 记录 | 属主 | 所属批次 |
|---|---|---|
| `BOOT_EPOCH`（类型 1，版本 1，置 `REQUIRED`；正文为 8 字节小端 `boot_epoch`） | `kernel` | 单独批次，开放输出前提交（C-ID-009、ADR-0016） |
| `WRITER_FENCE`（类型 10，版本 1，REQUIRED；正文 4 字节小端新 writer epoch，须与头部相等） | `durability` | 单独批次（C-STORE-005） |
| `OWNER_BINDING`（类型 12，版本 1，REQUIRED） | `authority/principal` | 首次所有者绑定，与启动领域 transition 同批；已绑定后不重复写入（ADR-0028） |
| `INSTANCE`（类型 2） | `work/agent` | 启动或改变实例的 transition 批次（ADR-0026） |
| `GOAL`（类型 3）、`GOAL_STATE`（类型 4） | `work/goal` | `GOAL` 在创建它的批次（B1）；`GOAL_STATE` 在每个改变该 Goal 的批次，含 occurrence 游标（B2），所以不另设 `OCCURRENCE_CURSOR` |
| `GOAL_AUTHORITY`（类型 17，版本 2/3，REQUIRED） | `work/goal` | v2 创建/批准；v3 授权暂停，与 4/v4 及直属活 Task 的暂停事实和清理进度同批；每批最多一个 Goal（ADR-0037、0038） |
| `TASK`（类型 5）、`TASK_STATE`（类型 6） | `work/task` | `TASK` 在派生它的批次（B2）；`TASK_STATE` 在每个改变该 Task 的批次，含 `TASK_PROGRESS` 的内容 |
| `TASK_AUTHORITY`（类型 18，版本 1，REQUIRED） | `work/task` | 派生 Task 时与确切 `TASK`/`TASK_STATE` 同批，状态后紧接准入事实（ADR-0038） |
| `WORK_AUTHORITY_CLEANUP`（类型 19，版本 1，REQUIRED） | `work/authority` | 工作清理请求/一步的批次末尾；快照固定一项，含明确 idle（ADR-0038） |
| `TASK_STEPS`（类型 7）、`TASK_CHECKPOINT`（类型 8） | `work/task` | 提交步骤或私有检查点的决策批次（B3），即 `STEP_CHECKPOINT` |
| `COMMAND_MAPPING`（类型 9，ordinary v1、create v2、revoke v3、approve v5、Task limit v6，REQUIRED） | `kernel/command` | 接受该命令的 transition 批次（B1）；v2/v3/v5含完整canonical请求，v3另存effect，v5保存原授权结果（ADR-0030、0031、0038） |
| `BUDGET_RESERVATION`、`BUDGET_SETTLEMENT` | 已废止（ADR-0042），未分配类型 | 不实现费用账本 |
| `CALL_IDENTITY` | `work/call` | B3 |
| `ACTION_STATE`（20/v1 codec，嵌入 CALL_STATE，不单独写 journal） | `work/action` | 通过 REQUIRED CALL_STATE 保留；准备、屏障与终态各自的批次，终态在 B4 |
| `CALL_RESULT` | `work/call` | B4 |
| `MEMORY`（23/v1）、`CONTEXT`（26/v1），REQUIRED | `knowledge` | 管理/策略变更，见 P3 记录布局 |
| `PROVENANCE`（24/v1、v2）、`PRIVACY`（25/v1），REQUIRED | `work/provenance`、`work/privacy` | 同批来源与独立清理进度，见 P3 记录布局 |
| `GRANT_CREATE`（类型13，版本1，REQUIRED） | `authority/grant` | Grant创建，与v2命令身份/结果同批（ADR-0030） |
| `GRANT_STATE`（类型14，版本1/2/3/4，REQUIRED） | `authority/grant` | v1 旧到期，v2 后代撤销，v3 专属 Goal 结束，v4 到期并要求工作清理；不持久 Gate |
| `GRANT_REVOCATION`（类型15，版本1/2，REQUIRED） | `authority/grant` | 显式根撤销或明确 no-op，与 v3 命令结果同批；v2 另要求工作清理 footer（ADR-0031、0038） |
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

类型 2 至 9 的 ordinary v1由 ADR-0026 定义，均置 `REQUIRED`；C 为创建命令新增类型9/version2，D为撤销新增version3，见下节。记录写对象在该 transition 之后的状态，不写操作，所以重放不重新执行规则、不调用策略、不读时钟。字节串为 `uint32` 长度加内容；目录 ID 至多 32 字节。

| 类型 | 记录 | 正文 | 长度 |
|---:|---|---|---:|
| 2 | `INSTANCE` | `agent_id` u64、`instance_id` u64、`revision` u32、`lifecycle` u32 | 24 |
| 3 | `GOAL` | `goal_id`、`parent_goal_id`、`owner_instance_id`、`replaced_goal_id`、origin `principal_id`（各 u64）；origin `kind`、`channel`、`binding_revision`、`depth`、`goal_type_version`（各 u32）；`goal_type_id` 字节串；trigger `kind` u32、`delay_us` u64、`period_us` u64、`trigger_source` 字节串；`expiry_after_us` u64、`occurrence_wait_max_us` u64；参数字节串 | 108 + 字节串 |
| 4 | `GOAL_STATE` | `goal_id`、`last_task_id`（各 u64）；`lifecycle`、`revision`、`cancel_revision`、`occurrences_skipped`、`skip_reason`、游标 `taken`、游标 `waiting`、`flags`（各 u32）。`flags` 位 0 = 还有下一次 occurrence，位 1 = 失效尚待发生；其余位须为 0 | 48 |
| 5 | `TASK` | `task_id`、`goal_id`、`owner_instance_id`、`replaced_task_id`（各 u64）；`occurrence`、`occurrence_count`（各 u32） | 40 |
| 6 | `TASK_STATE` | `task_id` u64；`lifecycle`、`revision`、`cancel_revision`、`hold_status`、`hold_reason`、`step_count`（各 u32） | 32 |
| 7 | `TASK_STEPS` | `task_id` u64、`steps_before` u32、`count` u32，然后 `count` 个 `step_key` 字节串：本 transition 追加的步骤，接在已有的 `steps_before` 个之后 | 16 + 字节串 |
| 8 | `TASK_CHECKPOINT` | `task_id` u64、写入方 `version` u32、写入方策略 ID 字节串、私有检查点字节串 | 20 + 字节串 |
| 9 | `COMMAND_MAPPING` | `principal_id`、`command_id`、内容摘要、该 principal 当时的下限（各 u64）；结果的意图 `kind`、对象 `kind`（各 u32）、对象 `id` u64、对象 `revision` u32。应用命令的 `commit_seq` 取自所在批次 | 52 |

- 记录不含单调时间（C-STORE-014）：Goal 写时长，不写到期时刻；Task 的 `due_us` 不写入。现有Goal/Task计时从恢复结束时刻起按完整时长重新计算（ADR-0026 决策6）；Grant限时恢复直接失效，绝不重锚（ADR-0030）。
- 一个批次内的次序：可选的首次 `OWNER_BINDING`，`INSTANCE`，D显式根`GRANT_REVOCATION`，`GRANT_CREATE/GRANT_STATE`，D根`GRANT_REVOCATION_PROGRESS`，新建 Goal 的 `GOAL`，各 `GOAL_STATE`，新建 Task 的 `TASK`，各 `TASK_STEPS`、`TASK_CHECKPOINT`，各 `TASK_STATE`，最后 `COMMAND_MAPPING`。新建的 Goal 或 Task 须在同一批次带有状态记录，否则为损坏。
- 状态记录引用的对象须已在表中，Goal 与 Task 的 `revision` 不减，`TASK_STEPS` 的 `steps_before` 须等于 Task 已有的步骤数，取代的对象须已终结；Goal 的 owner 须是此前记录启动的实例，终态的 Goal 与 Task 不再改变，带 `hold_status` 停下的 Task 只能结束，实例的 `revision` 不减、停止后不再改变；日志末尾不得有未终结的 Task 留在终态 Goal 之下，也不得有未终结的 Goal 留在终态的父目标之下或属于已停止的实例（结束 Goal 的 transition 同时结束它的后代与 Task，停止实例的 transition 同时结束它的 Goal）。违反者为损坏（C-STORE-009）。新建对象取代的对象记在 `GOAL`、`TASK` 中，重放按记录取代，不重新选择。本次启动的容量或限值放不下的记录使恢复以 `STATE_EXCEEDS_CAPACITY` 受限，损坏或容量失败受限时丢弃已重放的内容；所有者绑定失配保留已校验的诊断模型，不写入新批次（ADR-0028）。
- 不改变持久状态的 transition 写一个空批次，`commit_seq` 仍然连续。

## Grant 与完整授权命令记录（MA-006 C/D，验证状态见各自 ADR）

领域codec为`authority/grant/grant_records.*`和`kernel/command/command_records.*`。source_domain与scope均采用B的finite domain format 1，但前者保存原完整域，后者保存全部展开字段，必须在source_domain内；不依赖当前catalog即可验证持久事实。当前exact目录缺失/完整bytes不同在恢复结束后施加易失hold，不属于损坏，也不写入snapshot。

| 类型/版本 | 正文顺序 | 长度 |
|---|---|---:|
| 13/v1 `GRANT_CREATE` | grant_id、replaced_grant_id、subject_instance_id、parent_grant_id、owner_principal_id（u64）；owner_binding_revision、capability_version（u32）；capability_id字节串；lifetime、allow_delegation（u32）；duration_us（u64）；depth（u32）；source_domain字节串、expanded_scope字节串 | 80 + cap ID正文 + source_domain + scope；最大112+2×scope_max_bytes |
| 14/v1 `GRANT_STATE` | grant_id u64、revision u32、lifecycle u32 | 16 |
| 9/v2 `COMMAND_MAPPING` | v1的52 B正文，后接canonical_format u32、canonical_size u32、canonical正文 | 60 + canonical正文；C最大144+scope_max_bytes |
| 14/v2 `GRANT_STATE` | v1的16 B状态，后接revoked_by_root_id u64；必须REVOKED/revision2 | 24 |
| 14/v3、v4 `GRANT_STATE` | 原 16 B 状态；v3 必须 GOAL_ENDED/revision2，v4 必须 EXPIRED/revision2；同批 REQUIRED type19 | 16 |
| 15/v2 `GRANT_REVOCATION` | 原 v1 正文；同批 REQUIRED type19 | 32 |
| 15/v1 `GRANT_REVOCATION` | grant_id u64、revision u32、effect u32、initial_after_grant_id u64、through_grant_id u64 | 32 |
| 16/v1 `GRANT_REVOCATION_PROGRESS` | root_grant_id、after_grant_id、through_grant_id（u64×3） | 24 |
| 9/v3 `COMMAND_MAPPING` | v1的52 B正文，后接effect/canonical_format/canonical_size（u32×3）、kind10 canonical16 | 80 |

Grant创建初态ACTIVE/revision1，创建stamp取batch；到期state只能ACTIVE→EXPIRED/revision2，last stamp取batch。duration必须与PERMANENT=0/CURRENT_BOOT>0一致；不写monotonic deadline，创建boot从ID高32位得出。运行时deadline只在创建boot内存中比较；replay校验必要duration/boot/父关系，恢复的限时Grant一律失效，不从duration重新计时。普通step及首个启动领域transition显式结算到期；未持久终态不能成为回收候选。

replaced_grant_id精确记录安全终态叶节点的让位，不在replay重新选择。父必须存在，cap identity/source_domain与父一致，scope/再委派权限/期限只能收窄，depth按边数根0；恢复检查完整链及最终跨对象一致性。C journal的Grant创建与其v2 mapping必须同批匹配，核对owner/capability/subject/parent/期限/标志/expected_revision及GRANT revision1结果；raw scope在保存source_domain下展开须恰等保存scope，不能仅验证二者各自合法。缺少任一半或配对不一致为损坏，不能用可查询的孤立权限继续运行。

v2 canonical_format当前为1，必须有非空完整授权命令正文和GRANT/revision1结果，不接受Scene/Quota flags作为已受理内容，重算digest并以length+bytes确切比较；v1只兼容既有ordinary kinds，不可承载auth。原始命令身份不依赖当前catalog，具体见C-WIRE-017。原始canonical Grant命令最大固定部84 B：kind、expected_revision、capability_version（u32），cap ID字节串，subject/parent（u64），lifetime/flags（u32），duration（u64），原始scope字节串。raw scope头为u16format/u8provided_count/u8reserved0，每字段为u32id/u8kind/u8unit_length/u8member_count/u8reserved0，再接unit和I64端点或length+token；其上下文不同于B展开domain，保留省略，禁止混用reader。

新增auth record缺REQUIRED、未知版本、正文非法或跨批不完整均不能被跳过；replay、Boot discovery（含已被快照覆盖的历史）、snapshot共享分类。持久状态超过本次scope/Grant/canonical配置界时报告容量受限，不截断或降成digest。

### 撤销与恢复（ADR-0031，D Native 场景已验证，#28 已合并）

kind10 的 canonical 独立为16 B小端 `kind u32 / original_expected_revision u32 / target u64`，均要求对应非零合法值；v3仅承载revoke，effect为CHANGED/NO_CHANGE/TARGET_GONE，结果revision分别为2/2/0，重算digest并校验target。原始请求codec只要求expected非零，保留新请求expected999的CAS冲突语义；已接受v3 mapping的共享写/读/restore校验则要求原expected为1或2，对三个effect一律拒绝不可能的999。它不改变kind9 raw格式1。首次CAS接受后不再重检，不能强制CHANGED的原expected为1：未持久child清理接受expected2，reload为ACTIVE1后仍可合法应用CHANGED2。

15/v1与9/v3完整mapping/结果必须同批，owner/binding、target、原始canonical与effect必须一致。CHANGED将ACTIVE根置REVOKED/revision2、revoked_by_root_id=self，固定清理through和initial after；NO_CHANGE保留既有终态、stamp及cursor；TARGET_GONE只记录自动重载后消失的已接受目标，不生成Grant。逐步清理以保留根为依据，固定through、单调after；14/v2后代状态与本步16/v1进度同批，校验真实祖先关系及跨表一致性。首次终态stamp和进度cleanup_stamp独立；进度不能改变安全回收顺序。所有保留后代（含 ACTIVE、EXPIRED 与独立撤销根）的 ID 都不得超过其任一已撤销祖先根的 through；否则拒绝快照，不能让缩短的扫描上限提前结束清理。

Gate、pending、pin、queued boot/seq、monotonic deadline均不写日志或快照。自动reload先在各owner保留易失控制、清除可恢复事实，完整恢复后才以exact原mapping确认或重新应用；不能把公开durable floor当作原请求确认。全进程重启仅从持久事实恢复，不承诺保留PENDING。

journal与snapshot共享显式上下文分类；普通 Grant 的13/v2只用于snapshot；E3a 的专属 journal 13/v2 另有绑定尾部，不得互换，journal中不能当13/v1读。新增15/16、14/v2、9/v3以及covered历史都受REQUIRED/版本检查；旧reader必须受限，不能跳过新授权事实。

## 控制记录与读取

- `WRITER_FENCE` 单独成批，新 epoch 必须严格大于前缀现有有效 epoch。其后的完整旧 epoch 帧被忽略，不推进有效 commit、transition 或 boot 基线；其他有效批次仍须连续。
- `SNAPSHOT_COMMIT` 正文为 `slot` u32、`payload_crc` u32、`generation` u64、`covers_commit_seq` u64、`max_transition_id` u64，共 32 字节。它自身是覆盖边界之后的下一批次；维护也消耗统一 transition 身份。当前外壳只接受 schema 2；不再执行 schema 0→1 迁移。
- 恢复使用 Host 物理 cursor，不能用最大已见 commit_seq 翻页；旧 epoch 的高序号帧可能位于合法新批次之前。cursor 在同一日志实例中严格递增，轮转后从 0 重读。
- 首个完整批次放不下时，Host 以读专用 `READ_BUFFER_TOO_SMALL` 报精确所需长度，映射 `STATE_EXCEEDS_CAPACITY`。真正 I/O 故障、非法大小或 `OK` 越界仍是 `STORAGE_READ_FAILED`。

## 快照

快照采用两个槽。维护先冻结新 transition 并排空 journal，得到边界 `S`，再写非当前槽的内容，回读校验版本、长度与 CRC 后独立写槽尾标记。槽尾标记只证明槽内容完整；日志中的 `SNAPSHOT_COMMIT` 是激活新状态的唯一切换提交，持久化后才轮转旧日志。尚未激活的新槽不参与恢复。

### 槽头与槽尾（schema 2）

头部固定为 72 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYSN` |
| 4 | 2 | `storage_schema` | 2 |
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

正文沿用日志的 12 字节记录信封、类型编号与 `REQUIRED` 位，但**快照上下文的正文语法独立于 transition 日志**。通用 Grant writer 显式写13/v2且兼容读13/v1，专属 Grant 写13/v3（ADR-0037）；COMMAND_MAPPING按kind保留v1/v2/v3/v5/v6/v7/v8/v9；GOAL 快照写 v4，只接受此创建 stamp 完整格式，Task state 按运行限制/调用引用使用 v1/v4/v8（见末节）；其余下表均为版本1。不直接写C++布局；C/D验证状态见ADR-0030/0031。

| 类型 | 快照正文 | 长度 |
|---:|---|---:|
| 12 `OWNER_BINDING` | 日志正文，随后为建立绑定的原始 `transition_id` u64 与 `commit_seq` u64 | 32 |
| 2 `INSTANCE` | 日志的 `INSTANCE` 正文，随后为该实例最后改变的 `transition_id` u64 与 `commit_seq` u64 | 40 |
| 3/v4 `GOAL` | 日志的 `GOAL` 正文，随后为完整 `GOAL_STATE` 正文，再接原始创建 `transition_id`、创建 `commit_seq`、最后改变的 `commit_seq`（各 u64） | 日志 `GOAL` 长度 + 72 |
| 17/v2 `GOAL_AUTHORITY` | 与日志正文相同；保留自身最后 authority stamp，不追加另一组尾部 | 72 + 每需求 28 + ID/域/Scope 字节 |
| 5 `TASK` | 日志的 `TASK` 正文，随后为原始 `created_transition_id`、最后改变的 `commit_seq`、`steps_seq`、`checkpoint_seq`（各 u64），以及 `steps_before` u32 | 76 |
| 7 `TASK_STEPS` | 日志的步骤正文，每条只含一个 key，`steps_before` 为该 key 的位置、`count=1` | 20 + key 字节数 |
| 8 `TASK_CHECKPOINT` | 与日志正文相同，包含写入方策略 ID、版本及原检查点；没有写入方时不写该记录 | 日志正文长度 |
| 6 `TASK_STATE` | 与日志正文相同，在同一 Task 的全部步骤与检查点之后；v1 默认或 v4 运行限制 | 32 / 44 |
| 18/v1 `TASK_AUTHORITY` | 与日志正文相同，紧接该 Task 的状态；保留原始准入 stamp | 48 + 每引用 12 |
| 13 `GRANT_CREATE` | v1为创建正文+created/last stamp（各u64×2）+revision/lifecycle（u32×2）；v2再接revoked_by_root_id/cleanup_after/cleanup_through（u64×3）、cleanup stamp（u64×2）；v3使用专属创建正文并保留相同清理尾部；均无deadline/hold/Gate | v1正文+40；通用v2正文+80，最大192+2×scope_max_bytes；专属v3为绑定创建正文+80，最大200+2×scope_max_bytes |
| 9 `COMMAND_MAPPING` | 保留该条v1/v2/v3/v5/v6日志正文、effect及完整canonical/授权结果（如有），再接原接受批次的`commit_seq` u64 | v1为60；v2为日志正文长度+8；v3为88；v5为日志正文长度+8；v6为88 |

顺序为可选的单例 `OWNER_BINDING`、全部持久实例（不含 `instance_id=0` 的本次注册项）、按父先子后深度排序的Grant、按深度从小到大的 Goal（v2 后紧接 type 17，可跨页）、每个 Task 的元数据/逐条步骤/检查点/状态/准入（type18，紧接状态且允许跨页），最后按原接受 `commit_seq` 递增的命令映射。静态导入允许终态父子与已经不存在的 `replaced_goal_id`、`replaced_task_id`、`replaced_grant_id`前驱，仍检查 ID 唯一、owner、父子深度、版本、游标、状态与最终跨表一致性。原始创建身份、最新提交、步骤提交及检查点提交均保留，不改写成 `S`，以保持 READY 顺序、终态回收顺序与命令重试结果。

所有对象与命令的提交不超过 `S`，创建 transition 不超过快照基线；本次注册内容的缺失导致的 Agent fault、Goal hold、Task recovery hold 不写入快照。没有注册的 principal 仍占有原有的一行命令窗口及下限；第二个 principal 的持久命令放不下时明确报容量不足，不丢弃原行。命令窗口缩小时按原接受次序让位并提高下限。单调时刻不写入：Goal 只保留时长及是否待发生，Task不写`due_us`，这些工作计时最终恢复后重锚；Grant只写duration，恢复限时Grant失效，不能重锚。Grant保留created/last/cleanup stamp及终态顺序；三个stamp分别检查commit不超过S、transition不超过max_transition_id，非零cleanup独立检查，EXPIRED的last不能靠created界限替代。正常命令窗口可已淘汰创建mapping，snapshot不要求每个保留Grant仍有原mapping。

所有auth mapping核对OwnerBinding principal及接受seq、合法Kernel ID/boot，create继续校验完整不可变创建事实与scope。retained revoke CHANGED target必须为self-root REVOKED、终态seq等于mappingseq；NO_CHANGE target必须已终态且终态seq严格小于mappingseq；TARGET_GONE target必须缺席。已合法回收的CHANGED/NO_CHANGE target允许缺席。被根清理的保留后代ID不得超过根after，terminal stamp不得超过根cleanup stamp；revoker/after/through同样核对合法Kernel ID及snapshot boot界限；不改写为snapshot边界冒充原事实。

导出、回读与恢复按页复用同一存储缓冲，单条记录上限由配置容量计算，整份快照可以大于缓冲；Core 不分配另一份 WorkModel 或完整快照内存。任何损坏、不支持或容量不足均丢弃本次导入的全部可恢复状态，自动reload已接受的易失Gate/pending仍由各owner保留。绑定不匹配时，已完整校验的模型仅保留用于只读诊断。

### Goal 专属 Grant 的版本扩展

自 E3a（ADR-0037）起，通用 Grant 的旧字节保持不变。专属创建使用 REQUIRED type 13/v2：完整旧 type 13/v1 正文后接非零 bound_goal_id（u64，小端），该绑定必须为早于此 Grant ID 的合法 Kernel ID，且 parent_grant_id、flags 均为 0。绑定只允许该 Goal 消费，不能委派；不能用普通 GRANT_CREATE 的完整 canonical 命令证明此绑定。

专属快照使用 REQUIRED type 13/v3：上述创建正文加原 v2 的 80 B 状态/清理尾部；最大为 200 + 2×scope_max_bytes。通用快照仍写 v2 并可读历史 v1；journal v2 和 snapshot v2 含义不同，由上下文分类。新版本零绑定、旧版本多出绑定字节、版本与正文尾部不匹配都拒绝，不丢字段回退。E3b 通过下述完整批准配对开放公开专属创建。

### Goal 原子批准命令

E3b 原使用 REQUIRED type 9/v4；F4 前置格式改为 REQUIRED type 9/v5，在 v2 的 60 B 外壳后保留授权结果长度及正文，kind 必须 GOAL_APPROVE=11，result 为目标 Goal、expected_revision+1、CHANGED。canonical_format 仍为 1；正文按小端顺序为 kind/expected_revision（u32）、Goal ID（u64）、原缺项 mask/lifetime（u32）、duration（u64）、owner/parent（u64）、GoalType version（u32）、GoalType ID 字节串、原参数字节串、trigger kind（u32）、delay/period/原 expiry（u64）、trigger source 字节串。固定字段及三个 u32 长度合计 92 B，最大 156 + goal_parameter_max_bytes。旧 type 9/v1/v2/v3 不能承载 kind11，v4 不再是本轮活动存储支持版本。

批准批次必须包含恰好对应原缺项的 N 条 type13/v2、type4/v2 的 PROPOSED→ACTIVE、type17/v2 的 proposed→authorized、最后 type9/v5。新 Grant 必须为同 Goal、同主体、完整原 domain/Scope 的不可委派根；类型/域/Scope/期限与命令及缺项逐一匹配，旧引用不替换。Grant 可以按槽位顺序排列，但新 ID 与缺项引用必须是一一映射。缺少任一项、重复/额外 Grant、顺序错误或同批其他工作均拒绝；启动对新 Grant 创建 boot 先后关系的检查也适用 v2。

type9/v5 正文顺序为 v1 固定 52 B、canonical_format u32、canonical_size u32、canonical 正文、result_size u32、result 正文，总长 64 + canonical_size + result_size。批准请求 canonical 不变。result 固定 52 B：建立绑定的 transition_id、binding_commit_seq、parent_goal_id、parent_binding_commit_seq、input_incarnation、input_through（各 u64），再接 requirement_count u32；随后每项按原需求顺序保存 grant_id u64、revision u32（必须 ACTIVE revision1），共 52 + 12×count B，明确零需求也必须有完整结果。批准 input 两项为零；不从当前 Goal 引用重建历史结果。

快照保留 v5 完整原请求、结果及接受 seq。结果必须受快照 transition/boot 上界约束；当前 Goal 未换代时，引用和绑定须与当时结果一致，建立 stamp 尚未被后续暂停替换时须精确一致。每阶段授权事实 stamp 不早于不可变 Goal 创建 pair。普通 Goal 状态/元数据不能解除既有授权暂停；完整显式恢复另由 F4 公开事务交付。Goal 快照 v4 保存完整创建 pair，旧 Goal 快照和 v4 批准映射在新写前明确拒绝并保留，不推算创建序号或补结果。

批次容量预算包含 min(goal_requirement_capacity, grant_capacity) 条最大 Grant 创建记录。命令槽与 scratch 共用 stride，为 max(84+scope_max_bytes, 156+goal_parameter_max_bytes+52+12×goal_requirement_capacity)，Grant 命令无结果时不额外叠加授权结果预算。记录格式和 ABI 分别演进；本前置不新增公开 C 接口。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-001 | 批次按上表编码：头部、记录区、提交标记；`header_crc` 位于 `header_length − 4` 并覆盖其前的全部头部字节；`header_length` 小于 44 时批次不完整；`header_crc`、`payload_crc` 与 `commit_seq_low` 都校验通过才算完整 | 只检查头部 CRC，记录区短写的批次被当作完整 | M1 已实现（编解码）：`BatchCodec.every_damaged_byte_is_corrupt`、`BatchCodec.every_short_write_is_corrupt`、`BatchCodec.header_shorter_than_44_bytes_is_corrupt`、`BatchCodec.longer_header_from_a_later_writer_is_accepted`、`BatchCodec.commit_marker_of_another_commit_seq_is_corrupt`；`MA-010` 短写注入；E2 F7 |
| C-STORE-002 | 只有提交标记完整的批次可见；日志末尾不完整的批次被忽略，不部分应用。忽略尾部由 Host 负责（C-HOST-009）：Kernel 把读到的每个批次当作已提交，未通过校验的批次按 C-STORE-009 处理（ADR-0016） | 尾部批次只写了一半记录，恢复时应用了前半 | M1 已实现：Kernel 侧 `BatchCodec.every_short_write_is_corrupt`、`PrefixScan.damaged_batch_stops_the_scan`；Host 丢弃撕裂的尾部 `HostStorage.torn_tail_is_dropped_and_overwritten`、`NativeHost.torn_tail_is_truncated_before_the_next_append`；追加中断电的批次只在完整写入时计入 `WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；重放受限时丢弃已重放的全部内容 `WorkRecovery.log_beyond_the_capacities_restricts_and_shows_nothing`（ADR-0026）；E2 F0、F7、F8 |
| C-STORE-003 | `commit_seq` 从 1（有快照后从快照覆盖的下一个）起连续递增。Host 只交回完整批次，所以读到的缺口或与 Host 分帧不一致的 `commit_seq` 都是已提交区域损坏，按 C-STORE-009 处理（ADR-0016） | `commit_seq` 7 缺失，恢复跳过它继续应用 8 和 9；或把前缀截断在 7 之前，下一次 append 又写出一个 7，日志分叉 | M1 已实现（物理读取、连续前缀与栅栏）：`PrefixScan.commit_seq_must_continue_exactly`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`、`WriterFenceScan.fence_must_advance_the_writer_and_match_its_header`、`WorkRecovery.every_stored_prefix_recovers_the_state_after_its_last_batch`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；ADR-0027 |
| C-STORE-004 | 读到 `REQUIRED` 位置位的未知记录类型或版本、未知的 `flags` 位或不支持的 `storage_schema` 时进入 `RESTRICTED`；未置位的未知记录可以跳过 | 新固件写入的授权记录被旧固件跳过，旧固件按更宽的权限运行 | M1 已实现（恢复读取）：`BatchCodec.other_schema_or_flags_are_unsupported`、`PrefixScan.unknown_records_follow_the_required_flag`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`KernelLifecycle.unknown_optional_records_are_skipped`、`SnapshotFaults.an_unknown_required_record_in_a_covered_batch_still_stops_recovery`、`KernelLifecycle.an_optional_record_of_a_newer_version_is_skipped`；E4 步骤 6；C-VER-020 |
| C-STORE-005 | 受限栅栏确认旧写入方隔离后，写入 `WRITER_FENCE` 记录（含新 `writer_epoch`）；恢复时忽略栅栏记录之后出现的旧 epoch 批次 | 旧 flush 在重载后落盘，恢复时被当作新批次应用 | M1 已实现（Native 排空、Sim epoch 安装、旧写拒绝和物理扫描）：`KernelFence.exhausted_retries_reload_once_and_close_old_sessions`、`KernelFence.unavailable_isolation_preserves_diagnostics_without_new_boot`、`SimStorageFaults.fence_rejects_an_owned_late_write_after_its_unknown_completion`、`HostStorage.writer_fence_isolates_append_snapshot_and_trim`、`WriterFenceScan.stale_physical_batch_never_moves_the_logical_cursor`、`KernelFence.isolation_below_the_epoch_asked_for_restricts`；动作并存与执行端停止随 `MA-022`；E2 F12 |
| C-STORE-006 | 快照先写内容、回读校验后独立写槽尾标记；双槽与 `generation` 保留旧有效内容，只有绑定该槽的 REQUIRED `SNAPSHOT_COMMIT` 持久化后才激活并轮转。恢复只认最新有效激活绑定的槽，目标损坏或不支持时不回退旧槽 | 新槽内容尚未完整即覆盖唯一旧槽；或最新激活的槽损坏后按旧槽继续执行 | M1 已实现（槽格式、激活/轮转和发布阶段断电）：`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`、`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`；`SnapshotFaults.power_loss_at_each_publication_request_keeps_the_durable_work`、`SnapshotFaults.corrupt_or_unsupported_active_slot_never_falls_back_to_an_older_slot`、`SnapshotFaults.a_candidate_can_replace_a_superseded_slot_before_an_older_trim_finishes`、`SnapshotFaults.the_snapshot_after_a_recovered_one_takes_the_next_generation_and_slot`、`SnapshotCodec.an_activation_reads_back_only_what_a_writer_can_store`、`SnapshotCodec.boot_epoch_beyond_the_id_epoch_is_corrupt`；C-HOST-011 |
| C-STORE-007 | 恢复从最新激活绑定的快照开始，重放 `commit_seq > covers_commit_seq` 的已提交批次；同一 transition 不重复应用（C-COMMIT-002），对象原始 stamp、READY 顺序、回收顺序、命令结果与下限不被快照覆盖提交改写 | 快照已包含的批次再次应用，或全部对象改用同一覆盖提交，改变任务公平性与回收选择 | M1 已实现（现有工作模型的静态分页快照与尾部重放）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`WorkSnapshot.paged_round_trip_keeps_the_original_stamps_and_checkpoint`、`WorkSnapshot.ready_and_reclamation_choices_match_the_uncompacted_model`、`WorkSnapshot.terminal_parent_and_child_keep_their_reclamation_order`、`WorkSnapshot.a_retained_object_can_name_a_predecessor_already_reclaimed`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`、`WorkSnapshot.smaller_object_and_checkpoint_capacities_report_no_room`、`WorkSnapshot.smaller_goal_parameter_limit_is_capacity_failure`、`SnapshotFaults.a_snapshot_record_beyond_the_buffer_exceeds_capacity`、`WorkSnapshot.object_stamp_beyond_the_snapshot_boundary_is_corrupt`；其余领域随各自工作包；E2 F11 |
| C-STORE-008 | CRC 只用于发现损坏，不作为授权或防篡改依据 | 以 CRC 正确为由信任一条从外部导入的 Grant 记录 | 评审 |
| C-STORE-009 | 已提交区域（不是末尾的不完整批次）出现损坏时进入 `RESTRICTED`，不跳过损坏批次继续应用后续批次 | 中间批次 CRC 错误，恢复跳过它，导致后续撤销记录引用不存在的 Grant | M1 已实现（恢复读取）：`PrefixScan.damaged_batch_stops_the_scan`、`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`SnapshotFaults.payload_damage_the_records_survive_is_still_corrupt`；M1 已实现（工作记录违反表规则或日志末尾不一致时为 `LOG_CORRUPT`，已重放的内容全部丢弃，ADR-0026 决策 5）：`WorkRecovery.log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt`、`WorkReplay.goal_created_without_its_state_is_corrupt`、`WorkReplay.live_task_under_an_ended_goal_is_inconsistent`、`WorkReplay.live_goal_under_an_ended_parent_or_a_stopped_owner_is_inconsistent`、`GoalRestore.restore_state_refuses_a_change_to_a_terminal_goal_or_an_impossible_cursor`、`TaskRestore.restore_state_refuses_a_change_to_an_ended_or_held_task`、`AgentRestore.record_no_transition_writes_is_corrupt`；E2 F7（已提交区域损坏） |
| C-STORE-010 | 迁移在切换提交前保留旧格式；新 schema 快照经校验和槽尾确认后，以旧 schema 的 REQUIRED 激活批次切换，之后才允许新 schema 写入；中断前未激活的新槽可以重建，激活后不能恢复过时来源执行 | 原地改写旧日志导致新旧都不完整，或旧固件忽略新状态回到旧快照 | M1 部分实现（ADR-0038 开发版不提供迁移，旧格式无新写地受限并保留）：`StorageSchemaBoundary.unsupported_schema_never_installs_a_writer_or_mutates_source`、`StorageSchemaBoundary.unsupported_schema_is_still_rejected_after_a_process_restart`、`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`；E4 步骤 6；C-VER-021 |
| C-STORE-011 | 日志轮转、双槽快照、临时输出、历史大小与写频率均有上限；维护与限频暂时冻结新 transition 并报告背压，动作屏障不能为节省写入而跳过；实际空间不足时新副作用返回 `AYNI_STORAGE_FAILED`，已映射命令仍返回首次结果。保留当前可查询对象、步骤/检查点和既有幂等窗口，不额外归档已覆盖历史 | 为减少磨损跳过屏障，或轮转删除尚未激活快照覆盖的日志，或存储满使已接受命令失去原结果 | M1 部分实现（当前工作模型的轮转、保留、空间准入和写频；动作屏障尚未实现）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`StorageRecovery.retry_wait_is_readable_and_preserves_command_lookup_before_busy`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`；真实空间不足的准入 `StorageAdmission.full_medium_refuses_new_state_but_keeps_the_original_command_result`、`StorageAdmission.an_unmapped_noncommand_also_requires_space_before_applying`；日志上限与两项预留 `StorageBudget.default_log_limit_is_eight_buffers_or_one_mib`、`StorageBudget.smallest_log_limit_holds_the_reserve_two_batches_and_an_activation`、`StorageBudget.admission_needs_the_larger_reserve_free`、`StorageBudget.a_space_shortage_holds_transitions_until_a_refresh_finds_room`、`StorageBudget.a_snapshot_needs_its_reserve_and_a_shortage_holds_transitions`、`StorageBudget.compaction_is_due_at_three_quarters_of_the_limit`、`StorageBudget.compaction_is_due_before_the_log_reserve_would_cross_the_limit`、`StorageAdmission.a_log_limit_without_room_to_compact_is_refused`、`StorageAdmission.a_pending_compaction_holds_new_work_until_the_journal_drains`；持续限频 `SnapshotFaults.every_mutating_phase_shares_the_interval_and_waits_without_spinning`、`SnapshotFaults.a_slow_strategy_cannot_spend_the_next_storage_interval_before_submit`；动作屏障随 MA-008/MA-018/MA-022；E2 末段；C-COMMIT-015 |
| C-STORE-012 | 日志扇区擦除在没有活动执行器会话时预先完成；会话期间只做页编程，以免擦除挂起停止监督（C-STOP-005） | 会话中日志写满一个扇区，触发擦除，L2 心跳超时误停 | `MA-026` 故障矩阵 |
| C-STORE-013 | 参与 transition 的状态只写入日志；NVS 只保存不参与 transition 的小型配置。必须跨介质原子更新时，以日志中的协调记录实现（C-COMMIT-014） | Task 调用计数写在 NVS、Task 写在日志，掉电后只更新了一边 | M1 已实现（工作模型：实例、Goal、Task、步骤、私有检查点与命令映射只写入日志，每个 transition 一个批次，ADR-0026）：`WorkRecovery.power_loss_takes_the_batches_never_appended`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；Task 调用计数等其余状态随各自的包 |
| C-STORE-014 | 持久记录及快照不包含单调时间值、句柄、指针、资源占用或原始媒体（C-PRIM-001、C-TIME-003）；快照只保存持久领域视图，不把本次目录缺失造成的恢复 fault/hold 永久化 | 快照保存重启前单调时刻，或换回兼容策略后 Task 因上次恢复的临时 hold 仍不能继续 | M1 已实现（现有工作记录和快照：Goal 只写时长，Task 不写 `due_us`，恢复后重新计时）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`WorkSnapshot.bytes_are_independent_of_this_boots_monotonic_clock`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`；Grant 不含 deadline/易失 hold：`GrantRecords.duration_is_persistent_while_deadline_hold_and_transition_stamps_are_not`（ADR-0030）；评审 |
| C-STORE-015 | Grant创建与完整授权命令mapping/结果同批；保留source_domain、展开scope、父链、原始stamp和精确replacement，journal/snapshot均不写deadline/易失hold；unknown/missing REQUIRED的授权记录含covered历史均受限 | 孤立Grant被恢复为权限；快照遗漏原始域或把当前hold永久化；旧reader跳过授权新记录 | M1 已实现（Grant 创建/期限及完整命令的 journal/snapshot）：`GrantApi.interrupted_append_recovers_both_create_and_exact_mapping_or_neither`、`GrantReplay.neither_a_create_nor_an_exact_mapping_may_replay_without_its_partner`、`GrantReplay.new_authority_records_require_the_flag_even_for_unknown_future_versions`、`GrantReplay.snapshot_orders_a_reused_child_slot_after_its_parent_and_rejects_the_reverse`；ADR-0030 |
| C-STORE-016 | 显式根撤销与完整v3命令结果同批；有界后代状态与根进度同批，fixed through/单调after/真实父链及独立cleanup_stamp均可重放检查，精确replacement须为持久安全叶节点 | 根已撤销但丢命令结果；后代已改而cursor未改；cursor改变终态回收序 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.root_record_and_exact_mapping_require_each_other_in_one_batch`、`GrantRevokeReplay.descendant_state_and_progress_are_an_atomic_pair`、`GrantRevokeReplay.a_later_cursor_cannot_repair_a_child_state_paired_with_another_roots_progress`、`GrantRevokeReplay.descendant_and_progress_must_name_the_same_root_and_cursor`、`RevokeApi.only_a_durable_terminal_leaf_can_make_room_after_revocation_cleanup`；最终交付见 ADR-0031；C-COMMIT-005 |
| C-STORE-017 | snapshot显式写通用Grant13/v2并兼容v1，专属Grant写13/v3（ADR-0037），mapping1/2/3各按版本读；共享classifier区分journal/snapshot及covered历史，分别检查created/last/cleanup界和effect/retained target一致性，不保存Gate/pending/pin/deadline | snapshot13/v2被journal按v1解释；NO_CHANGE与同seq CHANGED互换；cleanup超过边界仍导入 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.snapshots_write_grant_v2_and_read_legacy_v1_without_inventing_revocations`、`GrantRevokeReplay.record_versions_are_specific_to_journal_and_snapshot_contexts`、`GrantRevokeReplay.snapshot_cleanup_stamp_has_independent_transition_and_commit_bounds`、`GrantRevokeReplay.snapshot_revoke_effect_must_agree_with_the_retained_target_history`、`GrantRevokeReplay.snapshot_cleanup_watermarks_cannot_name_a_future_boot`；最终交付见 ADR-0031；C-STORE-014 |
| C-STORE-018 | 有授权元数据的 GOAL v2/v3 与 type 17 必须配对，journal 同批且 stamp 相同、snapshot 相邻且在 Goal/快照边界内；原需求不可改写；活引用必须匹配已有 Grant 的主体、原域与范围 | 缺元数据的新 Goal 被当成旧零需求；跨 Goal 借记录；按当前目录重释旧范围 | M1 部分实现（内部事实、公开创建及 journal/snapshot/boot 集成，批准随 E3）：`GoalAuthorityPersistence.journal_round_trip_preserves_zero_and_rejects_missing_evidence`、`GoalAuthorityPersistence.missing_duplicate_or_wrong_batch_metadata_restricts_boot`、`GoalAuthorityPersistence.paged_snapshot_keeps_adjacent_metadata_and_original_stamps`、`GoalAuthorityPersistence.restored_updates_keep_the_original_ceiling_and_never_return_to_proposed`、`GoalAuthorityPersistence.a_grant_for_another_agent_cannot_authorize_the_restored_goal`、`GoalAuthorityPersistence.a_narrow_grant_does_not_cover_a_wider_saved_requirement`、`GoalAuthorityPersistence.proposed_snapshots_require_the_new_state_envelope`；ADR-0037 |
| C-STORE-019 | Goal 专属 Grant journal v2/snapshot v3 显式保存非零绑定；通用 Grant 旧字节不变；版本、根/不可委派及身份规则在 struct/bytes 共用校验 | 旧 reader 丢绑定；新 envelope 接受零绑定；普通命令替代批准证明 | M1 已实现（专属记录、版本边界和整批批准恢复）：`GoalGrantRecords.binding_round_trips_only_under_its_exact_record_versions`、`GoalGrantRecords.invalid_bound_facts_never_publish_partial_decoded_outputs`、`GrantRevokeReplay.record_versions_are_specific_to_journal_and_snapshot_contexts`、`GoalApprovalPersistence.public_approval_survives_journal_and_snapshot_with_exact_retry`、`GoalApprovalPersistence.missing_partner_or_mismatched_exact_command_restricts_recovery`、`GoalApprovalPersistence.snapshot_command_echo_cannot_disagree_with_retained_creation`、`GoalApprovalPersistence.unrelated_work_before_during_or_after_approval_restricts_boot`、`GoalApprovalPersistence.old_dependency_must_have_been_active_at_approval`、`GoalApprovalPersistence.one_new_grant_cannot_fill_two_equal_original_missing_items`、`GoalApprovalPersistence.maximum_missing_count_with_large_domains_fits_batch_reservation`、`WorkAuthorityResult.command_record_round_trip_and_all_truncations_are_atomic`、`WorkAuthorityResult.current_generation_result_keeps_its_establishment_stamp`、`GoalAuthorityPersistence.ordinary_metadata_cannot_clear_a_persisted_deny`、`GoalAuthorityPersistence.proposed_authority_cannot_precede_immutable_creation`、`GoalAuthorityPersistence.current_goal_sequence_cannot_exceed_snapshot_coverage`、`GoalApprovalPersistence.retained_result_cannot_substitute_another_covering_old_grant`、`WorkAuthorityResult.enclosing_mapping_sequence_must_match_the_saved_binding`、`WorkAuthorityResult.shared_creation_echo_keeps_distinct_nonzero_delay_and_period`；ADR-0037；补充验证：`WorkCommandCodec.approval_checks_each_shape_and_atomic_output_boundary` |

## Goal 授权元数据（MA-006 E/F）

type 17/v2 已接入 Kernel arena、journal、snapshot 和恢复分类，必须为 REQUIRED；公开需求创建与原子批准均已由 E 交付。GOAL type 3/v2、v3 复用 v1 正文并必须标 REQUIRED，声明该 Goal 有明确授权元数据：journal 同批在 GOAL_STATE 后写 type 17，snapshot 在 GOAL 后紧接 type 17（可跨页）；缺失、孤立、重复或跨批伙伴损坏。schema 2 不接受缺少元数据的 Goal；不从缺席推断零需求。每 transition 至多一条 type 17，显式零需求仍写该记录。

全部整数为小端。固定头 72 B：Goal ID（u64）、最后 authority transition_id（u64）、commit_seq（u64）、phase（u32，1=proposed、2=authorized、3=paused）、count（u32，0～16），再接五个 u64：binding_commit_seq（offset 32）、parent_goal_id（40）、parent_binding_commit_seq（48）、input_incarnation（56）、input_through（64）。随后按需求顺序重复：Capability version（u32），Capability ID、原域、Scope（三段各带 u32 长度的字节串），Grant ID（u64）、选定 revision（u32）。每项固定开销 28 B，不包括三段数据；总长必须恰好匹配。

Goal ID 必须是有效 Kernel ID，stamp 两项非零；Capability ID/版本与原域必须有效，Scope 完整且被原域包含。Grant 引用或是 `{0,0}`（缺项），或是有效 Kernel ID 与非零 revision。proposed 至少缺一项且 binding 全零；authorized/paused 全部引用完整，binding_commit_seq 非零且不晚于 authority commit_seq。父 Goal ID/绑定序号同零或同非零；根为零，子项须与准入时父绑定相等。输入 incarnation=0 时 through 必须为 0；非零 incarnation 可配零水位（无输入），不得超过 boot ID 范围。F2 的 journal/snapshot 均拒绝非零恢复边界，F4 随完整恢复事务证明另行开放。显式 count=0 可授权或暂停，缺少整条记录仍是未知元数据，不能当零需求。表的配置容量可以小于记录绝对上限，放入时不足则不改变已有事实。

journal/snapshot 使用同一 codec。journal 正文 stamp 等于 batch，Goal 状态必须在同批更新；已有元数据的原需求不可改变，stamp 严格前进，授权/暂停不能退回 proposed。snapshot stamp 在覆盖边界内且不晚于 Goal 的最后提交。Grant 选择保存 ACTIVE revision，与后来终态 revision 不同不构成损坏；所选主体、原 schema/domain 与覆盖关系仍须一致。活 Goal 所引用 Grant 缺失损坏，持久终态 Goal 可保留已回收历史引用。新 boot 的目录兼容与可消费性单独检查，不改写原需求。记录不保存单调时间、指针、进程句柄、输入队列状态或 Grant 的易失引用释放 fence。

自 E2b（ADR-0037）起，writer 对有元数据 Goal 写 type 3/v3（创建/快照），其 GOAL_STATE 写 type 4/v2（所有 journal 更新），均 REQUIRED。正文仍分别使用原布局；type 3/v3 允许 PROPOSED 和普通生命周期/授权暂停的独立组合，type 4/v2 同时保护已有 v2 Goal 的后续更新。旧记录 envelope 不代替 schema 2 必需的完整授权事实，不改变原 PAUSED 含义；type 4/v1 和旧版本快照不接受 PROPOSED。新格式 PROPOSED 必须有至少一项缺少引用的元数据，taken/waiting/next_scheduled/last_task_id 都为空，不能有子 Goal 或 Task。授权阶段 paused 可与普通 ACTIVE/PAUSED 同时存在；有效 PAUSED 由查询/消费派生，不另存一份。


## Task 授权元数据（MA-006 F）

ADR-0038 决策 2～6：type 18/v1 已接入 arena、journal、snapshot、发现及容量预算，必须 REQUIRED。journal 每批至多派生一个新 Task，按 TASK→TASK_STATE→TASK_AUTHORITY 配对，状态后必须紧接该 Task 的准入，批次结束不能留未配对身份；snapshot 在该 Task 的步骤/检查点/状态后紧接同一准入，可跨页。整批或整份快照拒绝缺项、重复、错 Task/Goal/绑定/引用/stamp、未来 boot 身份及夹带记录。旧 schema 在新写入前拒绝。

全部小端，固定头 48 B：offset 0 task_id u64、8 goal_id u64、16 goal_binding_seq u64、24 transition_id u64、32 commit_seq u64、40 phase u32（1=authorized、2=paused）、44 count u32。随后 count 项，每项 Grant ID u64/revision u32，共 12 B，顺序对应原 Goal 需求。count=0 是明确零需求，不是缺少事实。

Task/Goal 为不同的有效 Kernel ID；binding sequence 正且不晚于记录 commit_seq，transition_id 正；count≤16，每个引用为有效非零 ID/revision。reader 与 struct 共用语义验证；截断、尾字节、非法字段不改输出。表按稳定 Task.slot 和确切 task_id 放置，配置不足不改任何行。相同绑定只能保留同一引用，不能 paused→authorized；新绑定须单调，Task/Goal/需求数不变，stamp 两项严格推进。F2 在消费和重放复用同一 Task/Goal 精确匹配，活及终态 Task 都须保存相同 Goal 绑定、需求数及引用；Scope/父链检查沿用 Goal/Grant 属主。重新授权配对随 F4。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-020 | Task 保存独立完整准入引用，按稳定 slot/exact Task ID，显式零需求与缺席不同；struct/bytes 共用校验，错误不发布部分事实 | Task 位置移动或复用后借用另一 Task 权限；截断发布前缀 | M1 已实现（准入事实、持久配对、查询及显式重新授权）：`TaskAuthority.explicit_empty_admission_and_slot_reuse_preserve_identity`、`TaskAuthority.copied_references_and_capacity_failures_preserve_the_arena`、`TaskAuthority.same_binding_can_pause_but_cannot_resume_or_change_references`、`TaskAuthorityCodec.empty_and_maximum_admissions_round_trip_without_scope_copies`、`TaskAuthorityCodec.every_truncation_trailing_bytes_and_malformed_fact_preserve_output`、`TaskAuthorityPersistence.terminal_snapshot_rejects_missing_or_substituted_admission`、`WorkReplay.task_replacement_cannot_hide_a_pending_admission`；显式重新授权由 `GoalReauthorization.replaces_goal_and_ready_task_then_retries_without_reapplication` 覆盖；ADR-0038；补充验证：`WorkAuthoritySnapshot.every_retained_record_refuses_truncation_unknown_flags_and_versions`、`WorkAuthoritySnapshot.task_partners_refuse_individually_framed_identity_and_progress_mismatches` |


## 工作授权清理事实（MA-006 F3）

ADR-0038 决策 8：type 19/v1 固定 44 B，全部小端：transition_id u64（offset 0）、commit_seq u64（8）、phase u32（16，0=idle、1=scanning）、after_goal_id u64（20）、through_goal_id u64（28）、requested_through u64（36）。work 拥有状态和共享 reader/writer；Kernel journal、snapshot、发现及容量预算均接入 REQUIRED 记录，不从缺席推断清理完成。快照在 Task 区后、命令区前必须恰好保存一项，包括空表的明确 idle；开发期不为缺项旧快照补事实。

idle 的三个游标全部为零；初始 stamp 两项均零，使用后两项均正。scanning 的 stamp 为正，through 是固定有效 Goal ID，after 是零或小于 through 的有效已访 ID。requested_through 为零或下一轮的有效 Goal ID，即使等于当前 through 也须保留，不能因本轮结束丢失。空范围请求仅更新见证 stamp。稳定查询合并保留 Goal 和专属 Grant 的 Goal ID，包含 Goal 槽位已复用的旧身份；每步恰访最小未访 ID，或证明范围为空后结束，并保留下一轮请求。进度消耗 step item/time 预算，背压/快照冻结时暂停推进，即时消费检查继续拒绝失效权限。

授权暂停使用 GOAL_STATE 4/v4（原 48 B 正文）紧接 GOAL_AUTHORITY 17/v3（原 72 B 固定头），只允许既有活 Goal 从 authorized 进入 paused；用户 lifecycle/revision、失效标记和触发游标逐字段保持。直属活 Task 使用 TASK_STATE 6/v2（原 32 B 正文）紧接 TASK_AUTHORITY 18/v1，原用户/策略状态、revision、步骤和检查点保持，终态 Task 不得被更新。整批末尾的 type19 证明全部直属活 Task 已暂停，不能跨批补齐、覆盖待配对身份或在 footer 后夹带记录。目录兼容和主体不可用仍是独立 hold，不因后台扫描变成持久授权暂停。 journal 最终一致性与 snapshot 共用校验：每个直属活 Task 的授权 phase 必须与其自身 Goal 同为暂停或同未暂停；不把祖先尚待扫描的 phase 套给子 Goal，也不改写终态 Task 历史。旧暂停 journal 缺少独立 Task deny 时在新写入前拒绝并保留，不以用户 PAUSED 字段推断或补授权事实。

有 ACTIVE 专属 Grant 的 Goal 进入终态时使用 GOAL_STATE 4/v3（原正文），同批请求清理。扫描写 GRANT_STATE 14/v3（原 16 B 正文），lifecycle 为 GOAL_ENDED（4），只结束本步选中 Goal 的全部 ACTIVE 专属根，不改写既有终态顺序或伪造管理撤销；该 Goal 必须已于更早 transition 终结或已让出槽位。删除一项/全部结束记录、替换成另一 Goal 或仅推进游标均不构成完整一步。新到期记录 14/v4 和根撤销 15/v2 复用原正文，并要求同批 type19 请求；旧记录版本继续按其原配对规则读取。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-021 | 工作授权清理固定 through、单调 after，扫描中新请求保留给下一轮；记录与所属状态同批且有界推进，不丢清理请求 | 新请求扩大当前扫描导致不收敛；结束时丢掉同 frontier 的请求；槽位复用混入新 Goal | `WorkAuthorityCleanup.fixed_frontier_retains_requests_for_a_second_pass`、`WorkAuthorityCleanup.interrupted_scan_keeps_its_frontier_and_later_request`、`WorkAuthorityCleanup.dedicated_grant_ends_and_reclaims_after_goal_slot_reuse`、`WorkAuthorityPersistence.ended_grant_cohort_and_footer_are_one_complete_scan_step`、`WorkAuthorityPersistence.pause_keeps_user_state_and_requires_all_direct_task_facts`；ADR-0038 |


## 原子重新授权事实（MA-006 F4）

journal 使用 REQUIRED `GOAL_STATE` 4/v5、`GOAL_AUTHORITY` 17/v4、每个直属活 Task 的 `TASK_STATE` 6/v3 紧接 `TASK_AUTHORITY` 18/v1、`COMMAND_MAPPING` 9/v5，最后为请求清理的 19/v1。Goal 状态保留原用户/触发状态并严格增加一次 revision，只可按触发规则跳过积压；Task 状态保留 lifecycle、hold、步骤数和 checkpoint，严格增加一次 revision；元数据原 domain/Scope/count 不变，建立新代次并同时替换有序引用。一个 transition 只改指定 Goal 的完整直属活 cohort，其他记录不得前后夹带；command 完整回显、原双 CAS 与保存的结果必须精确配对。footer 只允许请求下一轮扫描，不能借此推进已有游标。

重新授权 canonical 固定 84 B 加两个 CatalogId 正文和参数：kind/revision u32、Goal ID/授权事实 CAS u64，再接批准共用的 60 B 原创建 echo；最大 148+goal_parameter_max_bytes，不超过批准输入预算。9/v5 保存本次不可变结果，input incarnation 为正；新引用必须来自此前已持久的 Grant，禁止批准专属根的同批发放例外。Goal 结束后的专属 Grant、父绑定和旧结果各按其事实属主验证，不能用当前引用反推历史命令。4/v5、17/v4、6/v3 仅用于 journal；snapshot 仍写 3/v4、17/v2、6/v1、18/v1 的完整当前事实与独立历史命令结果。

重放每条 Goal 授权事实时，从 BootSequence 借用该记录之前的已持久 boot 上界，要求 input incarnation 不超过该上界；自动重载可保留更早 ingress 实例，不要求等于当前 boot。后来的有效换绑、Goal 回收或 BOOT_EPOCH 不得追认历史中的未来输入实例。活 Task 必须精确匹配当前自身 Goal 的绑定/phase；终态 Task 可保留旧代的合法历史引用，已安全回收的 Grant 不补造记录。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STORE-022 | 重新授权的 Goal、全直属活 Task、原始双 CAS/回显、不可变结果与请求 footer 构成一个封闭批次；各前缀/重复/错配均拒绝，每条历史恢复边界受当时 boot 限制，快照保留新旧结果而不重建历史 | 合法 CRC 删一项 Task 或命令仍恢复；把清理步夹进请求；后续正常 R2 掩盖 R1 未来输入实例；同批新发 Grant 冒充已有授权 | M1 已实现（本地 C ABI 与持久场景）：`GoalReauthorizationPersistence.crc_valid_partner_faults_reject_the_whole_batch`、`GoalReauthorizationPersistence.every_complete_record_prefix_requires_all_partners`、`GoalReauthorizationPersistence.paged_snapshot_keeps_old_approval_and_new_binding`、`GoalReauthorizationPersistence.historical_result_stamp_boot_and_revision_are_bounded`、`GoalReauthorizationPersistence.later_binding_cannot_hide_a_future_journal_input_incarnation`、`GoalReauthorizationPersistence.replacement_cannot_issue_a_same_batch_dedicated_root`、`GoalReauthorization.interrupted_append_recovers_all_or_old_admissions`、`GoalReauthorization.failed_or_unknown_append_preserves_atomic_recovery_and_retry`、`GoalReauthorizationPersistence.later_task_cancellation_cannot_hide_an_omitted_live_cohort`、`GoalReauthorizationPersistence.replacement_cannot_resume_a_user_paused_goal_in_its_state`、`GoalReauthorizationPersistence.ordinary_task_state_cannot_follow_the_exact_command`、`GoalReauthorizationPersistence.management_footer_cannot_also_consume_a_cleanup_step`；ADR-0038；补充验证：`WorkCommandCodec.reauthorization_checks_each_shape_and_atomic_output_boundary`、`WorkAuthorityHistory.every_original_echo_field_is_checked_for_both_command_kinds` |

## Task 运行限制扩展（ADR-0043）

schema 2 不变。Task 保存 used/limit/blocked（模型调用次数、总软上限、独立限制暂停）。只在新请求超限时 blocked=1；used<=limit，1<=limit<=64，blocked 为 0/1 且为 1 时 used==limit。旧 type6/v1-v3 的默认事实为 0/8/0；只有这些默认值继续写旧版本，非默认值必须写 REQUIRED 新版本。

| 类型/版本 | 正文及配对 | 字节数 |
|---|---|---:|
| 6/v4 | v1 的 32 B 后接 used/limit/blocked（u32 各一个）；普通状态。快照用 v1 或 v4，仍紧接 Task authority | 44 |
| 6/v5 | 同 v4 正文；保持原 v2 的授权清理相邻配对，运行事实逐字段不变 | 44 |
| 6/v6 | 同 v4 正文；保持原 v3 的重新授权配对，运行事实逐字段不变 | 44 |
| 6/v7 | 同 v4 正文；只提高上限、清除该项暂停、revision+1，其他 Task 字段不变；必须为独立批次第一条并紧接精确所有者命令，批次不得夹带其他记录 | 44 |
| 9/v6 | v2 的 60 B 外壳（canonical_format=1）加 kind13 canonical；无授权 result。canonical 依次 kind u32、expected_revision u32、task_id u64、new_limit u32 | 80 |

快照保留 type9/v6 完整命令及原接受 commit_seq（共 88 B），Task 当前上限不必等于历史重试结果。journal 的 v7/v6 命令必须匹配同一 Task、原 CAS、新上限和绑定所有者，type6 v7 状态不能单独成立。普通状态不能降低 used、改变已有软上限或清除 blocked；用户 pause/resume 与授权变化保留该事实。新计数同调用身份的证明由 MA-008A 继续接入，不能用本轮 codec 测试宣称调用账本已实现。

低于原软上限的启动硬上限产生容量受限，不重置计数；旧 reader 对 REQUIRED 新版本拒绝，保留旧目录。所有 runtime codec、重放分发、快照大小与写入路径使用 `work/task/task_records` 的版本/布局实现；不存在逐平台私有编码。

## 统一调用记录（ABI 0.31，ADR-0045）

schema 2 保持；新 REQUIRED `CALL_STATE` type21/v1 为完整行。固定部分 288 B，加六个 CatalogId、每个资源的 8 B 固定部分及资源 ID、原域/输入/结果和可选嵌入 ActionState 正文。全部整数小端，字段按下列顺序，变长串均带 u32 字节数：

1. call_id、operation_id、task_id、owner_instance_id（各 u64）；Binding/Capability/Provider version（各 u32）；step_key、Binding ID、Capability ID、Provider ID、endpoint、destination（六个长度串）。
2. 原 CallContract：form/category/effects/idempotency/query/cancel/dedupe_window_ms/timeout_ms/input_format/result_format/input_text_limit/result_text_limit/resource_count（13 个 u32）；每个资源按规范 ID 顺序写 ID 长度串及 units u32。
3. created 和 latest stamp（各 boot/transition/commit_seq 三个 u64）；原执行 attempt 两个 u64；progress_sequence u64；cancel_cause、cancel_result、outcome、status、reason（各 u32，status 为有符号值的原位编码）。
4. 独立查询固定 60 B：generation/phase u32、requested stamp 三个 u64、查询 attempt 两个 u64、status/reason/conclusion u32。未请求查询时全部为零。phase 1/2/3 为 requested/dispatching/finished；generation 从 1 开始，管理 CAS 为 generation+1，无查询时 revision=1。原执行 attempt 不因查询改变。
5. 原 authority domain、规范 CallInput、UTF-8 结果、ActionState（四个长度串）；ActionState 在资源准备前为空，准备后使用既有 20/v1 codec，不另写 type20 记录。

Task 选定调用扩展使用 type6/v8/v9/v10/v11，分别保持 v1/v2/v3/v7 的配对含义；正文在 44 B 运行事实后附 logical_call_id u64 和 end u32，共 56 B。end 0/1/2 为 none/cancel/expire；无调用时 ID/end 同零。RUNNING/CANCELLING 必须有选定调用，后者还须 cancel/expire。终态 Call/Task 的同批更新由 replay 校验；取消后 retained UNKNOWN 可以没有 Task 选定引用。

type9/v7 使用 60 B 精确命令外壳，加 kind14/revision u32 与 call_id u64（16 B），共 76 B；快照另附原 commit_seq u64。查询受理的 CALL_STATE/管理命令同批相邻配对，保存的请求 stamp 不因查询完成而改变。查询结果与仍等待的 Task 同批；已取消 Task 只补调用事实，不写新的 Task 生命周期。

快照在 Tasks 后、authority cleanup/commands 前按稳定 call_id 保存全部调用。Body codec、journal 与 snapshot 共用表示和验证。每个 transition 最多改变一个 Call；容量预算含最大完整行及相应 Task/命令。新 reader 保留旧默认 Task 编码读取；旧 reader 遇 REQUIRED 新调用记录/状态版本显式拒绝，不修改旧目录。没有迁移工具。新的恢复、故障和中断清理专项仍未验证。

资源等待超时保留选定调用和模型次数，以 EXPIRED/RESOURCE_WAIT 叠加在原 Task 用户生命周期之上。普通 pause/resume 保留此原因；显式恢复使用 type6/v12（56 B，与 v8 相同字段），只清除此原因、revision+1，其他状态不变；紧随 type9/v8 的所有者命令。命令沿用 60 B 外壳与 16 B 规范 target 编码：kind15 u32、原 Task revision u32、task_id u64，共 76 B；快照另附原 commit_seq。v12/v8 必须为独立原子批次，不能单独恢复或夹带其他更新。快照仍以 v8 表达当前 Task 事实。

## ABI 0.33：工作分类与后台跳过（ADR-0047）

`GOAL` type3/v5（journal）、v6（snapshot）仅用于非默认 WorkProfile，均 REQUIRED。基础 body 保留 type3/v3 的字段顺序，在 parameters 字节串之后追加 u32 work_class、u64 background_period_min_us，共 12 B；固定长度由 108 B 增至 120 B。profile 的默认 ordinary/0 仍使用旧格式。v6 的创建 stamp、当前序号与 GoalState 尾沿 v4，追加 profile 位于基础 body 内，尾的位置跟随该 body 长度。类型和频率按同一 WorkProfile 校验；父/任务/授权配对规则保持，v5/v6 要求已有授权事实。

`CALL_STATE` type21/v2 与 v1 字节布局相同，仅用于 Core 专用 SKIPPED：status=EXPIRED、reason=RESOURCE_WAIT、attempt=0、正文为空、明确未派发；Action 可缺失，或为尚未递交的本地取消证明。跨表检查要求原 Goal 保存 BACKGROUND profile，同批 Task 为 EXPIRED 并移除选中调用，保留调用身份、接受次数和步骤占用。正常调用继续 v1；旧 reader 遇到新 REQUIRED 版本明确拒绝，不把未知结果改为跳过。


## ABI 0.35：Scene 与物理执行事实（ADR-0050）

schema 2 保持。新 Scene `type22/v1` 为 REQUIRED，journal/snapshot 共用完整表示，固定部分 44 B：object_id u64、Scene ID 长度串、definition_version/revision/mode 三个 u32、transition_id/commit_seq 两个 u64、原始声明长度串。mode=1 disabled、2 enabled。声明格式随 Scene v1 固定：rule_count/condition_count 两个 u32；各规则为 Capability ID 长度串、version u32、原完整 domain 与 Scope 两个长度串；随后各条件保存 source ID/version、kind/origin/unit、原 TTL、max_age、minimum/maximum（有符号原位编码）。实际固定长度由 owner codec 计算；字节池按启动容量预留，不存指针或本地 deadline。

Scene 的一次改变与精确 `type9/v9` 命令同批且无其他领域更新。命令沿用包含 effect 的 64 B 外壳；canonical 为 kind16/expected_revision/mode/definition_version 四个 u32 与 Scene ID 长度串（固定20 B）。快照命令另附原 commit_seq u64。第一次状态 revision=1 且使用当前 boot 新 Kernel ID；后续保持 object_id、revision+1。关闭保留原声明；NO_CHANGE 只写新命令、保留 Scene revision。快照在调用后、清理/命令前保存 Scene 行，恢复读取不依赖当前注册声明；消费时才检查完整配置一致性。

物理 `CALL_STATE type21/v3` 在 v1 的 ActionState 长度串之后追加 ExecutionFact；非物理保留 v1，后台未派发跳过保留 v2。尾部固定112 B，加三个 ID 字符串的正文、各数值投影固定16 B+字段/单位正文、各观察引用固定32 B+来源ID正文。按顺序为：Binding ID/version、Executor ID/version、Scene ID/version、form/lease_us/max_command_age_us/field_count；各字段 name/unit 两个长度串与 minimum_field_id/maximum_field_id u32；scene_enable_seq/control_domain u64、control_mode u32、Kernel/authority/Executor/safety 四个 epoch u64、calibration_version u32、phase/target/stop_cause/dependency_count 四个 u32；各依赖 source ID 长度串、version u32、boot/generation/sequence 三个 u64。

phase=1 PREPARED、2 DISPATCHING、3 STOPPED、4 NOT_STARTED；target=0 UNVERIFIED、1 SATISFIED、2 UNSATISFIED。已停止事实不能回到运行；原约束、代际和依赖不能改变。完整数值从原 canonical input 提取，不再复制另一份参数。nonce、deadline、轮询时间、在线端口诊断不持久；permit_id 取原执行 attempt.request_id，不另分配或恢复新 ID。执行停止可先独立提交，结果/Task 步骤仍同批，直到必要停止与借用清理完成才可回收。新增记录正常 codec 与功能证据不等于新恢复/中断专项已经验证。

## ABI 0.37：类型化工具调用（ADR-0054）

schema 2 保持。结果格式为 TOOL_CALLS 的调用契约，其每一行（含尚未完成、失败与 SKIPPED）都写 REQUIRED `CALL_STATE type21/v4`：v1 的完整布局后接 u32 结果种类，1 为 UTF-8 文本、2 为工具调用列表；种类 2 只能是 `COMPLETED`，结果长度串里是下述列表。TOOL_CALLS 只用于非物理模型契约，所以 v4 不与 v3 的执行尾部同时出现。旧 reader 遇到 v4 明确拒绝，不会把列表当作文本读出。

工具调用列表格式 1（全部整数小端，长度串为 u32 字节数加正文）：调用数 u32（1～8）；每个调用依次为 Binding version u32（非零）、Binding ID 长度串、字段数 u32（≤ 8）、各字段、正文长度串；每个字段依次为字段 ID u32、种类 u32、单位长度串，I64 再接 i64 值（原位编码），ENUM/IDENTIFIER 再接取值长度串。字段 ID 非零且在调用内严格递增；ID、单位与取值都是 CatalogId，I64 必须有单位，枚举与标识符的单位为空；正文为最长 65536 B 的 UTF-8。解码严格：任何越界、非规范顺序、未知种类或尾部多余字节都使整行无效。列表大小受该调用契约的 result_text_limit 约束，与文本结果相同；编码不存指针，也不保存模型原始 JSON。

## P3 记忆与内容来源记录（ADR-0060，ABI 0.40）

schema 仍为 2。以下新类型/版本全部 REQUIRED；不了解它们的 reader 受限并保留原目录，不能忽略来源继续输出。除注明快照专用外，journal/snapshot 共用属主编解码；整数小端，无 native padding。

**共享来源列表**：`count:u32` 后接 count 个 52 字节引用，顺序为 `kind:u32, id:u64, revision:u32, actor_instance_id:u64, purpose:u32, read_grant_id:u64, read_revision:u32, egress_grant_id:u64, egress_revision:u32`。kind 为 memory=1/context=2；无 Grant 使用 ID/revision 同为 0。格式上限 8；当前配置更小归为容量不足，非法引用仍归损坏。完整闭包包含实际权限，不允许同一身份的不同版本替换合并。

| 类型/版本 | 记录体与配对 |
|---|---|
| 23/v1 MEMORY | 32 字节头：`id:u64, revision:u32, state:u32, transition_id:u64, commit_seq:u64`；后接 MemoryValue：`namespace:u32, namespace_id:u64, source:u32, profile_field:u32, confirmed:u32, source_call_id:u64`，再接长度前缀 body、tag_count 和各 tag、keyword_count 和各 keyword、共享来源列表。值固定开销 48 字节，包含四个长度/count 字段。管理变更与 exact 命令同批；策略写与 Task checkpoint/provenance 同批。删除/修改另需同批 privacy 请求。 |
| 24/v1 PROVENANCE | 32 字节头：`work_id:u64, kind:u32, content:u32, transition_id:u64, commit_seq:u64`，后接共享来源列表。kind 为 Goal=1/Task=2；content 为 available=1/erased=2。available 必须与 Goal 创建或 Task checkpoint 的 stamp 精确配对；erased 只作为快照表示，正文/依赖均为空。 |
| 24/v2 PROVENANCE | v1 后附 `origin_context_id:u64, origin_file_generation:u64`；仅有完整 primary anchor 证据的 Goal 使用，Task 仍由 Goal 继承原文件基础。 |
| 25/v1 PRIVACY | 固定 48 字节：`request_seq:u64, phase:u32, effect:u32, cursor:u64, snapshot_generation:u64, transition_id:u64, commit_seq:u64`。请求与来源失效同批；普通进度单独批次且只改变一个属主表示。 |
| 26/v1 CONTEXT | 固定 80 字节后接共享来源列表：`id:u64, owner_instance_id:u64, revision:u32, pending_action:u32, generation:u64, last_turn:u64, old_revision:u32, reserved:u32=0, turn_id:u64, task_id:u64, transition_id:u64, commit_seq:u64`。OPEN/PUBLISH/FORGET/ACK 必须有精确命令伙伴；FORGET 另需 privacy 请求。清理诱发的 Context 擦除仅由 privacy 进度记录表达。 |

Goal 3/v7 journal、3/v8 snapshot 保留原工作布局，并始终含 12 字节 profile 尾部；版本明确要求后续来源记录，v8 还保留原 created stamp。Task 5/v2 snapshot 和 checkpoint 8/v2 的正文布局与原版相同，但明确要求 type 24 来源伙伴；没有 footer 的待定工作不能在恢复结束时发布。旧普通工作继续使用原版本。

命令 type 9：v10 Memory、v12 Context 均使用既有 52 字节映射加两个 u32（authority_command_format、canonical size）的 60 字节 exact 包络。Memory canonical 共用 MemoryValue 编码；Context canonical 固定 56 字节，由 kind/expected_revision、owner/context/generation/task/turn、action/reserved 组成。v11 是 snapshot-only 的 52 字节 privacy-expired 身份，digest=0、不保留 canonical/result 副本；同身份重试返回 EXPIRED，不匹配新的正文。v13 是一个非递归包装：`base_version:u32, base_size:u32, 原版本完整 body, 共享来源列表`；保存 Goal 命令来源独立于目标 Goal 生命周期。快照沿用既有额外 8 字节原 commit_seq 表达，不把当前快照序号当原始命令身份。

Call 21/v5 是正文已擦除的显式表示，用于快照及擦除后晚到完成/查询的 journal：复用原 Call 前缀、输入/结果长度、动作事实，正文长度为 0；action 后追加 `execution_present:u32` 与 24 字节 content_erased ActionStamp（boot_epoch、transition_id、commit_seq），有 execution 时再写原执行尾部。保留原 outcome/latest 及真实物理动作事实；工具正文不另留 v4 列表。晚到完成或查询先按原结果契约分类，再抑制正文，不能因隐私擦除改写执行结论。

清理 phase：memories=1、contexts=2、goals=3、tasks=4、calls=5、commands=6、files=7、compact=8、purge=9、complete=10；idle=0 不序列化。effect：none=0、memory=1、context=2、goal=3、task=4、call=5、command=6、remove_memory=7。cursor 是对象稳定 ID；命令阶段使用原 commit_seq。Context effect 之后唯一允许的回退是 memories/cursor=0/effect=none，直至来源稳定；其他普通进度不能倒退。files→compact 须所有 Context ACK 且外部借用释放；目标 snapshot_generation 为当前代际+2，溢出明确停止并报告容量失败。purge/complete 快照不能宣称超过本快照 generation 的轮转已完成。scrubbed Memory 从新快照省略；erased Provenance 保留不可用身份标记，正文/依赖为空，运行时完成双槽覆盖后移除 tombstone。

**Native 外层文件**不属于 Core schema：AYNF v3 固定 32 字节头为 `magic:u32, version:u32, encoded_size:u64, pending_goal:u64, context_id:u64`。正文 AYNC v2 为 magic/version 后的 Conversation 字段：7 个 u64（id、owner、隐私 generation、文件 revision、last_round、summary_first、summary_last），长度前缀 summary、独立 draft、round_count:u32，再逐轮 number:u64/user/answer。AYNT v2 固定头 28 字节（magic、version、done、base_revision、memory_id）后接相同 Conversation 字段和本轮 input。独立草稿与 pending_goal 同次保存；发布前必须保存完毕。正常保留 3 轮、额外 1 槽承接失效发布原始输入；原轮号不重新分配。旧 AYNF/AYNC v1 在 Kernel 启动写入之前拒绝；AYNF v2 是双 Agent 版本的目录（布局与 v3 相同，Kernel 状态含第二个 Agent），同样在启动写入之前以 UNSUPPORTED_VERSION 拒绝（ADR-0072）；均无自动迁移。


## P4 日历记录（ADR-0061，ABI 0.41）

存储 schema 仍为 2。新增类型/版本均 REQUIRED；不认识的 reader 在写入或执行前拒绝并保留原目录。

- **type 27/v1 CALENDAR**：64 字节固定部分后接 TZ 字节。依次为 `goal_id:u64, transition_id:u64, commit_seq:u64, last_local_s:u64, last_utc_s:u64, taken:u32`，再接规则 `kind:u32, rule_version:u32, first_local_s:u64, timezone_length:u32, timezone_bytes`。未消费的两个 last 字段编码为 UINT64_MAX，其他时刻范围 0～253402300799 秒。TZ 长度 1～128；规则 kind 1/2/3 为单次/每日/每周，rule_version=1。日志和快照共用正文；仅规则创建或消费/跳过时更新 stamp。Goal 槽退役同时清空本行。单调投影、首次容量等待时刻和时钟可信状态均不持久化。
- **type 3/v9、v10、v11、v12**：分别为无来源日历 Goal 的 journal/snapshot，以及带来源日历 Goal 的 journal/snapshot。沿用原 Goal 正文，始终携带 12 字节 work profile；快照保留原状态/creation stamp/current sequence 尾部。各版本要求对应 CALENDAR 行，v11/v12 另要求来源伙伴。GoalState 的 next_scheduled 位对日历恒为 0，日历剩余工作由规则与游标证明，恢复后重新投影。
- **type 5/v3、v4**：日历 Task 创建使用 v3，快照有来源时用 v4；原 40 字节创建正文追加 `calendar_local_s:u64, calendar_utc_s:u64`，共 56 字节。快照再追加原 36 字节历史尾部，共 92 字节。v1/v2 只接受原布局，v3/v4 只接受新布局，不能按长度猜测版本。Task 的 occurrence 顺序计数必须对应该日历规则的计划本地键；旧非日历 Task 不改写。
- **管理命令原始 Goal echo**：仅 CALENDAR_* 在原创建字段之后追加上述完整规则值（20 字节固定部分加 TZ）；普通 Goal 编码不变。审批/重新授权比较完整规则，不能仅比较日历 kind 或当前系统时区。

完整事务须保持 Calendar 游标、Goal 状态、Task 创建与初始元数据同批；周期重新授权跳过积压的 CALENDAR 伙伴限定同一 Goal 和 stamp。实际故障/恢复矩阵仍未验收，正常重开证据与最终测试结果见对应 PR。

## Task 订阅与等待（ABI 0.43，ADR-0063）

schema 2 保持，新增版本均 REQUIRED，旧 reader 显式拒绝并保留目录。CALL_STATE type21/v6 复用 v1 完整前缀及 action 后追加订阅尾；v7 使用 v5 擦除布局再追加同一尾。尾依次 source 的 u32 长度/字节，source_version、lease_ms、phase、desired、attachment 各 u32，generation、lease_sequence 各 u64，control_status、control_reason 各 u32：固定48 B 加 source正文。phase 1..6 对应公共枚举，desired 1/2=open/closed，attachment 1/2=foreground/background。generation 是全局 Kernel ID、也作为当前代 action.operation；初代等于原 operation，后代只在已知停止且旧 borrow 返回后替换旧 action/attempt。无 monotonic 截止、占用或 raw media 持久化。

STARTED 的 foreground→background、开启动作步骤和 Task 前台释放同批；之后后台回执不要求更新 Task.current_call。journal 仍每批至多一条 Call，Task/authority 变化逐 Call 收敛。正常快照/日志跨表校验拥有者、前后台引用与 Task 外键，UNKNOWN 阻止不安全回收。物理 Call 使用 scoped 观察时写 type21/v8（原v3）或v9（原v5擦除），每条观察依赖在原 source/version/boot/generation/sequence 后追加 subscription_id u64，0为Host；旧v3/v5读取为Host引用。

TASK_STATE type6/v13/v14/v15/v16 对应原v1/v2/v3/v7 的普通、授权、重新授权及提高模型上限配对规则；在56 B选定调用布局后追加 wait_subscription_id、generation、sequence 三个u64与 phase u32，共84 B。只在有等待前沿时使用，call_id/end同零，lifecycle只能WAITING或其用户PAUSED；其他版本不能表达WAITING。snapshot仅用v13。等待所指 Call 必须属同一Task且有订阅事实，前沿代际不得晚于当前代际；状态/序号允许落后以在下一owner步唤醒。普通无等待行继续使用原版本，唤醒/终态清除等待；所有授权/模型元数据更新保留它。正常公开重开验证见 `SubscriptionFixture.orderly_restart_of_waiting_task_retains_frontier_and_follows_new_generation`；不替代新的故障/恢复专项。

## P5 暂停组记录（ADR-0065，ABI 0.44）

- REQUIRED `PAUSE_GROUP` type28/v1，正文 56 B：`group` u32、`paused` u32（0/1）、稳定 `object_id` u64、`revision` u32、reserved u32（0）、`transition_id` u64、`commit_seq` u64、逻辑输入 `incarnation` u64、`through` u64。编号 1～8；对象/版本/stamp 正，输入前沿复用 C-AUTH 的逻辑格式，不存单调时间。默认开放且未控制的组不写记录；没有新建/回收/重用组 ID 的路径。
- 日志组事实排在所有工作/日历变化后及 COMMAND_MAPPING 前，必须与本批确切所有者命令配对；快照通过 PersistentStore 完整恢复每个已控制组，之后才结算 Task。
- REQUIRED `GOAL` type3/v13 日志、v14 快照沿 profile 布局追加 `pause_group` u32 和 `provenance_required` u32（0/1）；组必须非零且在固定范围。日历规则由触发种类及原 type27 配对证明；profile 总是存在。v14 继续保留原 state/creation stamp/current sequence 尾部。无组 Goal 继续使用原版本。
- REQUIRED `COMMAND_MAPPING` type9/v14 复用 exact v2 包络，canonical 16 B：kind=24 u32、expected_revision u32、group u32、paused u32。result revision 是 expected+1，effect=CHANGED。相同状态的新命令也推进一次控制 revision；同命令重试只返回首次结果。
- Goal 审批/重新授权的 canonical echo 仅在非零组时末尾追加 group u32；旧无组字节与普通创建 digest 保持。含组新事实使旧 reader 遇 REQUIRED 新版本时拒绝，保留目录；无迁移工具。
