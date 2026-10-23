# 持久记录格式

- **关联**：计划 §2.8、§4.2「持久化」、§4.3、§4.4「升级」；ADR-0002、ADR-0016、ADR-0026、ADR-0027、ADR-0030、ADR-0031；`MA-010`、`MA-006`；E2 F0–F8、F11、F12，E4 步骤 6
- **属主**：`durability`（批次编码、持久前缀、快照、重放、栅栏记录、迁移机制）；领域记录的内容由各属主模块定义，`durability` 不理解其语义
- **状态**：M1 当前写 `storage_schema=1`，明确读取 0、1；0 为迁移来源。布局以本文件和编解码测试为准，迁移切换见 ADR-0027；Native/模拟证据不代表 ESP32 或真实 OTA 验证；C 的 Grant/完整命令扩展及 D 的撤销扩展各自验证与交付状态见 ADR-0030/0031；D 已通过 Native 场景、覆盖率与内存量测，#28 已合并

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
| `TASK`（类型 5）、`TASK_STATE`（类型 6） | `work/task` | `TASK` 在派生它的批次（B2）；`TASK_STATE` 在每个改变该 Task 的批次，含 `TASK_PROGRESS` 的内容 |
| `TASK_STEPS`（类型 7）、`TASK_CHECKPOINT`（类型 8） | `work/task` | 提交步骤或私有检查点的决策批次（B3），即 `STEP_CHECKPOINT` |
| `COMMAND_MAPPING`（类型 9，ordinary v1、create v2、revoke v3，REQUIRED） | `kernel/command` | 接受该命令的 transition 批次（B1）；v2/v3含完整canonical请求，v3另存effect（ADR-0030、0031） |
| `BUDGET_RESERVATION`、`BUDGET_SETTLEMENT` | `budget` | B2、B4 |
| `CALL_IDENTITY` | `work/call` | B3 |
| `ACTION_STATE` | `work/action` | 准备、屏障与终态各自的批次；终态在 B4 |
| `CALL_RESULT` | `work/call` | B4 |
| `MEMORY_RECORD`、`MEMORY_TOMBSTONE`、`LINEAGE` | `knowledge` | B4、B5 |
| `GRANT_CREATE`（类型13，版本1，REQUIRED） | `authority/grant` | Grant创建，与v2命令身份/结果同批（ADR-0030） |
| `GRANT_STATE`（类型14，版本1/2，REQUIRED） | `authority/grant` | v1到期，v2后代撤销；不持久Gate |
| `GRANT_REVOCATION`（类型15，版本1，REQUIRED） | `authority/grant` | 显式根撤销或明确no-op，与v3命令结果同批B5（ADR-0031） |
| `GRANT_REVOCATION_PROGRESS`（类型16，版本1，REQUIRED） | `authority/grant` | 根cursor与本步后代状态（如有）同批B5（ADR-0031） |
| `INVALIDATION_CURSOR` | `knowledge/lineage` | B5 |
| `SNAPSHOT_COMMIT`（类型 11，版本 1，REQUIRED；正文见下） | `durability` | 快照切换 |

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

journal与snapshot共享显式上下文分类；13/v2只允许snapshot，journal中不能当13/v1读。新增15/16、14/v2、9/v3以及covered历史都受REQUIRED/版本检查；旧reader必须受限，不能跳过新授权事实。

## 控制记录与读取

- `WRITER_FENCE` 单独成批，新 epoch 必须严格大于前缀现有有效 epoch。其后的完整旧 epoch 帧被忽略，不推进有效 commit、transition 或 boot 基线；其他有效批次仍须连续。
- `SNAPSHOT_COMMIT` 正文为 `slot` u32、`payload_crc` u32、`generation` u64、`covers_commit_seq` u64、`max_transition_id` u64，共 32 字节。它自身是覆盖边界之后的下一批次；维护也消耗统一 transition 身份。迁移激活以 schema 0 外壳写入，之后正常写 schema 1，旧 reader 因 REQUIRED 类型未知而受限。
- 恢复使用 Host 物理 cursor，不能用最大已见 commit_seq 翻页；旧 epoch 的高序号帧可能位于合法新批次之前。cursor 在同一日志实例中严格递增，轮转后从 0 重读。
- 首个完整批次放不下时，Host 以读专用 `READ_BUFFER_TOO_SMALL` 报精确所需长度，映射 `STATE_EXCEEDS_CAPACITY`。真正 I/O 故障、非法大小或 `OK` 越界仍是 `STORAGE_READ_FAILED`。

## 快照

快照采用两个槽。维护先冻结新 transition 并排空 journal，得到边界 `S`，再写非当前槽的内容，回读校验版本、长度与 CRC 后独立写槽尾标记。槽尾标记只证明槽内容完整；日志中的 `SNAPSHOT_COMMIT` 是激活新状态的唯一切换提交，持久化后才轮转旧日志。尚未激活的新槽不参与恢复。

### 槽头与槽尾（schema 1）

头部固定为 72 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `AYSN` |
| 4 | 2 | `storage_schema` | 1 |
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

正文沿用日志的 12 字节记录信封、类型编号与 `REQUIRED` 位，但**快照上下文的正文语法独立于 transition 日志**。Grant writer显式写13/v2且兼容读13/v1；COMMAND_MAPPING按kind保留v1/v2/v3，其余下表均为版本1。不直接写C++布局；C/D验证状态见ADR-0030/0031。

| 类型 | 快照正文 | 长度 |
|---:|---|---:|
| 12 `OWNER_BINDING` | 日志正文，随后为建立绑定的原始 `transition_id` u64 与 `commit_seq` u64 | 32 |
| 2 `INSTANCE` | 日志的 `INSTANCE` 正文，随后为该实例最后改变的 `transition_id` u64 与 `commit_seq` u64 | 40 |
| 3 `GOAL` | 日志的 `GOAL` 正文，随后为完整 `GOAL_STATE` 正文，再接原始 `created_transition_id` u64、最后改变的 `commit_seq` u64 | 日志 `GOAL` 长度 + 64 |
| 5 `TASK` | 日志的 `TASK` 正文，随后为原始 `created_transition_id`、最后改变的 `commit_seq`、`steps_seq`、`checkpoint_seq`（各 u64），以及 `steps_before` u32 | 76 |
| 7 `TASK_STEPS` | 日志的步骤正文，每条只含一个 key，`steps_before` 为该 key 的位置、`count=1` | 20 + key 字节数 |
| 8 `TASK_CHECKPOINT` | 与日志正文相同，包含写入方策略 ID、版本及原检查点；没有写入方时不写该记录 | 日志正文长度 |
| 6 `TASK_STATE` | 与日志正文相同，在同一 Task 的全部步骤与检查点之后 | 32 |
| 13 `GRANT_CREATE` | v1为创建正文+created/last stamp（各u64×2）+revision/lifecycle（u32×2）；v2再接revoked_by_root_id/cleanup_after/cleanup_through（u64×3）、cleanup stamp（u64×2）；均无deadline/hold/Gate | v1正文+40；v2正文+80，最大192+2×scope_max_bytes |
| 9 `COMMAND_MAPPING` | 保留该条v1/v2/v3日志正文、effect及完整canonical（如有），再接原接受批次的`commit_seq` u64 | v1为60；v2为日志正文长度+8；v3为88 |

顺序为可选的单例 `OWNER_BINDING`、全部持久实例（不含 `instance_id=0` 的本次注册项）、按父先子后深度排序的Grant、按深度从小到大的Goal、每个 Task 的元数据/逐条步骤/检查点/状态，最后按原接受 `commit_seq` 递增的命令映射。静态导入允许终态父子与已经不存在的 `replaced_goal_id`、`replaced_task_id`、`replaced_grant_id`前驱，仍检查 ID 唯一、owner、父子深度、版本、游标、状态与最终跨表一致性。原始创建身份、最新提交、步骤提交及检查点提交均保留，不改写成 `S`，以保持 READY 顺序、终态回收顺序与命令重试结果。

所有对象与命令的提交不超过 `S`，创建 transition 不超过快照基线；本次注册内容的缺失导致的 Agent fault、Goal hold、Task recovery hold 不写入快照。没有注册的 principal 仍占有原有的一行命令窗口及下限；第二个 principal 的持久命令放不下时明确报容量不足，不丢弃原行。命令窗口缩小时按原接受次序让位并提高下限。单调时刻不写入：Goal 只保留时长及是否待发生，Task不写`due_us`，这些工作计时最终恢复后重锚；Grant只写duration，恢复限时Grant失效，不能重锚。Grant保留created/last/cleanup stamp及终态顺序；三个stamp分别检查commit不超过S、transition不超过max_transition_id，非零cleanup独立检查，EXPIRED的last不能靠created界限替代。正常命令窗口可已淘汰创建mapping，snapshot不要求每个保留Grant仍有原mapping。

所有auth mapping核对OwnerBinding principal及接受seq、合法Kernel ID/boot，create继续校验完整不可变创建事实与scope。retained revoke CHANGED target必须为self-root REVOKED、终态seq等于mappingseq；NO_CHANGE target必须已终态且终态seq严格小于mappingseq；TARGET_GONE target必须缺席。已合法回收的CHANGED/NO_CHANGE target允许缺席。被根清理的保留后代ID不得超过根after，terminal stamp不得超过根cleanup stamp；revoker/after/through同样核对合法Kernel ID及snapshot boot界限；不改写为snapshot边界冒充原事实。

导出、回读与恢复按页复用同一存储缓冲，单条记录上限由配置容量计算，整份快照可以大于缓冲；Core 不分配另一份 WorkModel 或完整快照内存。任何损坏、不支持或容量不足均丢弃本次导入的全部可恢复状态，自动reload已接受的易失Gate/pending仍由各owner保留。绑定不匹配时，已完整校验的模型仅保留用于只读诊断。

### 激活、轮转与恢复

`SNAPSHOT_COMMIT` 绑定槽、generation、覆盖提交、最大 transition 与正文 CRC；它的批次使用 `commit_seq=S+1`、`transition_id=max_transition_id+1`。schema 0→1 迁移的这个激活批次仍用 schema 0 并置 `REQUIRED`，切换前保留原日志；旧 reader 不能忽略激活记录回到过时状态继续执行。空存储直接建立 schema 1 的栅栏和启动记录，不制造空快照或迁移激活。

恢复先按物理日志次序发现有效 writer fence 与最新激活，再校验并导入它所指的确切槽，最后从 `S+1` 的激活批次起重放尾部。旧激活引用的槽可能已被复用，不逐条读取旧槽；最新激活的槽损坏或不支持时不得回退旧槽。被 fence 排除的旧 epoch 批次不改变激活、提交、transition 或 boot 基线。

激活持久后，轮转保留该激活及之后的物理日志帧；A 保留策略不额外归档已经覆盖的历史。双槽、当前日志与轮转临时输出均有上限；达到维护阈值时冻结新 transition，查询及已映射命令重试仍可读取。轮转中断不撤销已激活的新状态。

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
| C-STORE-010 | 迁移在切换提交前保留旧格式；新 schema 快照经校验和槽尾确认后，以旧 schema 的 REQUIRED 激活批次切换，之后才允许新 schema 写入；中断前未激活的新槽可以重建，激活后不能恢复过时来源执行 | 原地改写旧日志导致新旧都不完整，或旧固件忽略新状态回到旧快照 | M1 已实现（schema 0→1 迁移、切换前保留源和模拟旧 reader 拒绝切换）：`StorageMigration.schema_zero_work_migrates_and_the_old_reader_rejects_the_switch`、`StorageMigration.every_early_request_can_lose_power_without_losing_the_legacy_state`、`SnapshotCodec.every_header_and_marker_byte_is_checked`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`；E4 步骤 6；C-VER-021 |
| C-STORE-011 | 日志轮转、双槽快照、临时输出、历史大小与写频率均有上限；维护与限频暂时冻结新 transition 并报告背压，动作屏障不能为节省写入而跳过；实际空间不足时新副作用返回 `AYNI_STORAGE_FAILED`，已映射命令仍返回首次结果。保留当前可查询对象、步骤/检查点和既有幂等窗口，不额外归档已覆盖历史 | 为减少磨损跳过屏障，或轮转删除尚未激活快照覆盖的日志，或存储满使已接受命令失去原结果 | M1 部分实现（当前工作模型的轮转、保留、空间准入和写频；动作屏障尚未实现）：`StorageRecovery.automatic_snapshot_rotates_log_and_keeps_current_command_window`、`StorageRecovery.retry_wait_is_readable_and_preserves_command_lookup_before_busy`、`WorkSnapshot.commands_keep_their_floor_and_original_acceptance_order`；真实空间不足的准入 `StorageAdmission.full_medium_refuses_new_state_but_keeps_the_original_command_result`、`StorageAdmission.an_unmapped_noncommand_also_requires_space_before_applying`；日志上限与两项预留 `StorageBudget.default_log_limit_is_eight_buffers_or_one_mib`、`StorageBudget.smallest_log_limit_holds_the_reserve_two_batches_and_an_activation`、`StorageBudget.admission_needs_the_larger_reserve_free`、`StorageBudget.a_space_shortage_holds_transitions_until_a_refresh_finds_room`、`StorageBudget.a_snapshot_needs_its_reserve_and_a_shortage_holds_transitions`、`StorageBudget.compaction_is_due_at_three_quarters_of_the_limit`、`StorageBudget.compaction_is_due_before_the_log_reserve_would_cross_the_limit`、`StorageAdmission.a_log_limit_without_room_to_compact_is_refused`、`StorageAdmission.a_pending_compaction_holds_new_work_until_the_journal_drains`；持续限频 `SnapshotFaults.every_mutating_phase_shares_the_interval_and_waits_without_spinning`、`SnapshotFaults.a_slow_strategy_cannot_spend_the_next_storage_interval_before_submit`；动作屏障随 MA-008/MA-018/MA-022；E2 末段；C-COMMIT-015 |
| C-STORE-012 | 日志扇区擦除在没有活动执行器会话时预先完成；会话期间只做页编程，以免擦除挂起停止监督（C-STOP-005） | 会话中日志写满一个扇区，触发擦除，L2 心跳超时误停 | `MA-026` 故障矩阵 |
| C-STORE-013 | 参与 transition 的状态只写入日志；NVS 只保存不参与 transition 的小型配置。必须跨介质原子更新时，以日志中的协调记录实现（C-COMMIT-014） | 预算写在 NVS、Task 写在日志，掉电后只更新了一边 | M1 已实现（工作模型：实例、Goal、Task、步骤、私有检查点与命令映射只写入日志，每个 transition 一个批次，ADR-0026）：`WorkRecovery.power_loss_takes_the_batches_never_appended`、`WorkRecovery.append_cut_by_power_loss_counts_only_when_whole`；预算等其余状态随各自的包 |
| C-STORE-014 | 持久记录及快照不包含单调时间值、句柄、指针、资源占用或原始媒体（C-PRIM-001、C-TIME-003）；快照只保存持久领域视图，不把本次目录缺失造成的恢复 fault/hold 永久化 | 快照保存重启前单调时刻，或换回兼容策略后 Task 因上次恢复的临时 hold 仍不能继续 | M1 已实现（现有工作记录和快照：Goal 只写时长，Task 不写 `due_us`，恢复后重新计时）：`WorkRecovery.timers_restart_from_the_recovery_clock_with_their_full_length`、`WorkSnapshot.bytes_are_independent_of_this_boots_monotonic_clock`、`WorkSnapshot.compatibility_faults_and_holds_are_not_written`；Grant 不含 deadline/易失 hold：`GrantRecords.duration_is_persistent_while_deadline_hold_and_transition_stamps_are_not`（ADR-0030）；评审 |
| C-STORE-015 | Grant创建与完整授权命令mapping/结果同批；保留source_domain、展开scope、父链、原始stamp和精确replacement，journal/snapshot均不写deadline/易失hold；unknown/missing REQUIRED的授权记录含covered历史均受限 | 孤立Grant被恢复为权限；快照遗漏原始域或把当前hold永久化；旧reader跳过授权新记录 | M1 已实现（Grant 创建/期限及完整命令的 journal/snapshot）：`GrantApi.interrupted_append_recovers_both_create_and_exact_mapping_or_neither`、`GrantReplay.neither_a_create_nor_an_exact_mapping_may_replay_without_its_partner`、`GrantReplay.new_authority_records_require_the_flag_even_for_unknown_future_versions`、`GrantReplay.snapshot_orders_a_reused_child_slot_after_its_parent_and_rejects_the_reverse`；ADR-0030 |
| C-STORE-016 | 显式根撤销与完整v3命令结果同批；有界后代状态与根进度同批，fixed through/单调after/真实父链及独立cleanup_stamp均可重放检查，精确replacement须为持久安全叶节点 | 根已撤销但丢命令结果；后代已改而cursor未改；cursor改变终态回收序 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.root_record_and_exact_mapping_require_each_other_in_one_batch`、`GrantRevokeReplay.descendant_state_and_progress_are_an_atomic_pair`、`GrantRevokeReplay.a_later_cursor_cannot_repair_a_child_state_paired_with_another_roots_progress`、`GrantRevokeReplay.descendant_and_progress_must_name_the_same_root_and_cursor`、`RevokeApi.only_a_durable_terminal_leaf_can_make_room_after_revocation_cleanup`；最终交付见 ADR-0031；C-COMMIT-005 |
| C-STORE-017 | snapshot显式写Grant13/v2并兼容v1，mapping1/2/3各按版本读；共享classifier区分journal/snapshot及covered历史，分别检查created/last/cleanup界和effect/retained target一致性，不保存Gate/pending/pin/deadline | snapshot13/v2被journal按v1解释；NO_CHANGE与同seq CHANGED互换；cleanup超过边界仍导入 | M1 已实现并通过 Native 场景：`GrantRevokeReplay.snapshots_write_grant_v2_and_read_legacy_v1_without_inventing_revocations`、`GrantRevokeReplay.record_versions_are_specific_to_journal_and_snapshot_contexts`、`GrantRevokeReplay.snapshot_cleanup_stamp_has_independent_transition_and_commit_bounds`、`GrantRevokeReplay.snapshot_revoke_effect_must_agree_with_the_retained_target_history`、`GrantRevokeReplay.snapshot_cleanup_watermarks_cannot_name_a_future_boot`；最终交付见 ADR-0031；C-STORE-014 |

## Goal 授权元数据（MA-006 E，内部 codec）

type 17/v1 已接入 Kernel arena、journal、snapshot 和恢复分类，必须为 REQUIRED；公开需求/批准入口仍待后续 E。GOAL type 3/v2 复用 v1 正文并必须标 REQUIRED，声明该 Goal 有明确授权元数据：journal 同批在 GOAL_STATE 后写 type 17，snapshot 在 GOAL 后紧接 type 17（可跨页）；缺失、孤立、重复或跨批伙伴损坏。type 3/v1 缺元数据仍合法且继续 LEGACY_UNCONFIRMED。每 transition 至多一条 type 17，显式零需求仍写该记录。

全部整数为小端。固定头 32 B：Goal ID（u64）、最后 authority transition_id（u64）、commit_seq（u64）、phase（u32，1=proposed、2=authorized、3=paused）、count（u32，0～16）。随后按需求顺序重复：Capability version（u32），Capability ID、原域、Scope（三段各带 u32 长度的字节串），Grant ID（u64）、选定 revision（u32）。每项固定开销 28 B，不包括三段数据；总长必须恰好匹配。

Goal ID 必须是有效 Kernel ID，stamp 两项非零；Capability ID/版本与原域必须有效，Scope 完整且被原域包含。Grant 引用或是 `{0,0}`（缺项），或是有效 Kernel ID 与非零 revision。proposed 至少缺一项；authorized/paused 全部引用完整。显式 count=0 可授权或暂停，缺少整条记录仍是未知元数据，不能当零需求。表的配置容量可以小于记录绝对上限，放入时不足则不改变已有事实。

journal/snapshot 使用同一 codec。journal 正文 stamp 等于 batch，Goal 状态必须在同批更新；已有元数据的原需求不可改变，stamp 严格前进，授权/暂停不能退回 proposed。snapshot stamp 在覆盖边界内且不晚于 Goal 的最后提交。Grant 选择保存 ACTIVE revision，与后来终态 revision 不同不构成损坏；所选主体、原 schema/domain 与覆盖关系仍须一致。活 Goal 所引用 Grant 缺失损坏，持久终态 Goal 可保留已回收历史引用。新 boot 的目录兼容与可消费性单独检查，不改写原需求。记录不保存单调时间、指针、进程句柄、输入队列状态或 Grant 的易失引用释放 fence。
