# ADR-0017：工作模型第一部分：主体与会话、Agent 实例、GoalType 与 Goal 意图

- **状态**：已接受（`MA-004` 第一部分在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §2.2、§2.4、§2.7、§3.2、§3.6、§5.3、§14.1；`MA-004`、`MA-006`、`MA-010`；E2 F13；ADR-0006、ADR-0008、ADR-0009、ADR-0014、ADR-0016

## 背景

`MA-004` 覆盖 Principal/Session、Agent/Goal/Task、步骤调度与暂停取消，一个 PR 装不下，按依赖拆成三部分：

- **第一部分（本 ADR）**：主体与会话、Agent 实例、GoalType 注册、Goal 的创建/暂停/恢复/取消意图与查询。
- **第二部分**：Task、occurrence 与触发、`ayni_post_event`、Task 生命周期与传播矩阵、实例暂停/恢复/停止、“暂停助理”作用的集合、失效与有界等待（`next_deadline_us`）、owner 生命周期检查。它再拆为 Task 与传播、时间、事件三个 PR，见 ADR-0018；“暂停助理”作用的集合按计划 §9.1 交给 `MA-019`。
- **第三部分**：策略与 `decide` 循环、检查点、步骤、策略意图、`blocked_on`、C-LIFE-024。

第一部分要先回答几个问题：意图以什么形式进入 Kernel（ADR-0009 尚未决定）；Kernel 分配的 ID 怎样跨重启不重复，而持久化计数器要等 `MA-010`；在 Grant（`MA-006`）之前谁可以提交意图；日志记录（`MA-010`）落地之前，已接受的 transition 怎样如实报告“尚未持久”。

## 决策

1. **本地 API 用类型化 C 结构（ABI 0.5）。** 用户在 M1 选定 ADR-0009 的方案 (a)：本地调用直接传类型化结构，IDL 与线格式在 ESP32 实测后、最迟在 `MA-038` 编写消息解码器之前选定。`ayni.h` 新增：
   - 注册：`ayni_register_principal`（只有 `OWNER` 一种，每台设备一个，C-PROD-001）、`ayni_register_agent`、`ayni_register_goal_type`，只在 `CONFIGURING` 中有效。
   - 会话：`ayni_open_session`、`ayni_close_session`。
   - 意图：一个信封 `ayni_intent_v1`（`kind`、`command_id`、`session_id`、`expected_revision`、`body`），本部分实现 `GOAL_CREATE`、`GOAL_CANCEL`、`GOAL_PAUSE`、`GOAL_RESUME` 四种；`ayni_submit_intent` 另有可选的 `ayni_intent_result_v1` 与 `ayni_error_v1`（原因码与对象引用，C-STATUS-006）。
   - 查询：`ayni_get_agent`、`ayni_get_goal`；`ayni_kernel_info_v1` 追加 `durable_seq`。
   - `ayni_config_v1` 追加 7 项容量与限额（`contracts/domain/limits.md`）。

   0.5 新增的结构体只有一个布局，`struct_size` 必须等于其 `sizeof`；`ayni_kernel_info_v1` 仍以 0.4 的 24 字节为首个布局（C-VER-003、C-VER-004）。草案 `types_v1.h` 删除：用到的类型移入 `ayni.h`，其余类型随第一个使用者定义。其他计划中的意图种类留在草案 `kernel_api.h`，在实现之前 `ayni_submit_intent` 对它们返回 `AYNI_UNSUPPORTED`。
2. **Kernel ID 以启动代际为前缀（C-ID-010）。** `instance_id`、`goal_id`、`session_id` 共用一个分配器，取值为 `boot_epoch × 2^32 + seq`，`seq` 从 1 开始。`boot_epoch` 在进入 `RUNNING` 之前已经持久化（C-ID-009），所以不同启动的 ID 天然不同，不需要每次分配都写日志。代价是两个上限：
   - 每次启动最多分配 2^32−1 个 ID，用尽后新的 Goal 与会话返回 `CAPACITY_EXCEEDED`（`ID_EXHAUSTED`），重启后取得新的范围；每秒分配 100 个也要约 1.36 年才用尽。
   - `boot_epoch` 以 2^32−1 为上限：日志中的最大代际已到上限时以 `BOOT_EPOCH_EXHAUSTED` 进入 `RESTRICTED`。这修订了 ADR-0016 的 `UINT64_MAX` 上限；每分钟重启一次也要八千年以上才会到达。

   同时，日志中出现大于 2^63−1 的 `transition_id` 按 `LOG_CORRUPT` 处理（修订 ADR-0016 的 `UINT64_MAX`）：每次转换只取下一个 id，以每秒一百万次计算，正常写入方也要约 29 万年才到达这个值，于是本次启动可用的 id 远多于它能用掉的，内存中的 transition 计数器不会回绕。
3. **transition 先只在内存中应用（直到 `MA-010`）。** 每个被接受的意图是一个 transition：取启动批次之后的下一个 `transition_id` 与 `commit_seq`，立即应用到唯一的内存状态（C-COMMIT-003），但还不写日志。`durable_seq` 如实报告持久前缀：恢复期间是已读取的日志末尾，进入 `RUNNING` 后是启动批次；结果未知的追加不计入。因此每个对象的 `commit_seq` 都大于 `durable_seq`，调用方能看出它尚未持久（C-COMMIT-004）。这些位置没有存下，之后的启动从自己的启动批次之后重新编号，把同一 `commit_seq` 交给另一个 transition，所以 `commit_seq` 只能与同一次启动的 `durable_seq` 比较（C-COMMIT-016）；`MA-010` 之后，崩溃前没有写成的尾部同样如此。重启会丢失这些 Goal，实例也会取得新的 `instance_id`，`WorkModel.a_reboot_assigns_new_ids_and_forgets_unstored_work` 固定了这一现状；Goal 与实例记录、重放和命令幂等随 `MA-010`。
4. **实例随 Kernel 进入 `RUNNING` 而创建（C-LIFE-025）。** 进入 `RUNNING` 时，在一个 transition 中给每个已注册的 Agent 分配 `instance_id` 并转为 `RUNNING`；没有 Agent 时不产生 transition。策略尚未接入，所以暂不经过 `STARTING`（第三部分）。
5. **Goal 规则（`lifecycles.md`）。**
   - `GOAL_CREATE` 创建 `ACTIVE`、版本 1 的 Goal；需要确认的 `PROPOSED` 随审批流程（`MA-006`）。owner 必须是本 Kernel 的实例，之后不变（C-LIFE-020）。
   - 子目标的父目标必须存在、处于 `ACTIVE` 或 `PAUSED`、与子目标同一 owner；子目标深度不超过 `goal_depth_max`，父目标的未终结子目标少于 `goal_children_max`（C-LIMIT-007）。
   - `GOAL_PAUSE`、`GOAL_RESUME` 只改变目标本身。暂停可以撤回：若暂停级联，恢复父目标时就要区分哪些子目标是随父目标暂停的、哪些是用户单独暂停的，得为每个子目标记录暂停原因。取消是终态，没有这个问题，所以 `GOAL_CANCEL` 在同一 transition 中取消目标及其全部未终结的后代，并记录 `cancel_revision`（C-LIFE-022）。
   - 每个被改变的 Goal 版本加 1，其余不变。`revision` 是 `uint32_t` 且不回绕：要改变的 Goal 中有版本已到 2^32−1 的，整个意图返回 `CAPACITY_EXCEEDED`（`REVISION_EXHAUSTED`），取消时也检查每个后代（C-COMMIT-012）。
   - `goal_capacity` 计入终态 Goal。Goal 从不移除，所以下标与创建顺序始终有效：父目标总排在后代之前，取消的级联只需向后扫描一遍。回收终态 Goal 需要 tombstone 与保留策略（C-ID-001），随 `MA-010`。（自 ADR-0025 起，Goal 表满时最早结束的终态 Goal 让位给新 Goal，其他 Goal 不移动，位置不再是创建顺序，级联改为遍历整张表；ID 不复用，所以不留 tombstone。）
6. **检查顺序固定，拒绝没有副作用。** `ayni_submit_intent` 依次检查：参数与结构体大小；`UNSUPPORTED`（先意图种类，后非零 `command_id`）；从 GoalType 校验内调用时 `INVALID_STATE`，不在 `RUNNING` 时 `NOT_READY`，不排队（C-LIFE-004）；会话不存在时 `DENIED`；最后是意图自身的规则。
   - 未实现的种类在任何状态下都不受支持，放在 `NOT_READY` 之前，调用方就不会等到 `RUNNING` 再重试。
   - 命令幂等映射随 `MA-010`；在此之前接受并忽略 `command_id`，重发的命令就会悄悄创建第二个目标（C-WIRE-003），所以显式拒绝。
   - `GOAL_CREATE` 依次检查 GoalType 与版本、owner、父目标规则、容量与 ID，最后检查参数：先看大小，再调用 GoalType 的 `validate`。调用方代码放在最后，只看到其余条件都满足的 Goal，也不会影响前面原因码的顺序。
   - 变化类意图依次检查目标是否存在、`expected_revision`、生命周期、版本上限。
   - 任何拒绝都不取 ID、不取 transition，也不改变状态；`out_error` 在自身 `struct_size` 合法后每次都写，`out_result` 只在 `AYNI_OK` 时写。
7. **会话属于 `authority`，授权先只看会话。** primitives.md 把 UserPrincipal 与 Session 归 `authority`，所以会话表放在 `authority/principal/`；ADR-0008 中的 `work/session` 是控制会话（ControlSession，`permits_and_stop.md`），与此无关。Host 认证主体与渠道（`VOICE`、`MANAGEMENT`）后打开会话；意图只带 `session_id`，Goal 的来源（`USER`、渠道、主体、`binding_revision`）取自会话，意图无法自报（C-AUTH-010）。`ayni_open_session` 是公开函数，Kernel 分辨不出调用者是谁，所以本部分只保证意图不能自报主体与渠道；只让 Host 打开会话、不把这个入口交给应用，随 `MA-006` 与组合根一起实现。会话不持久化，重启后全部关闭。Grant（`MA-006`）之前，所有者的任何已打开会话都可以提交全部 Goal 意图；`MA-006` 在检查顺序第 4 步加入 Grant 与渠道信任级别的检查。
8. **GoalType 校验在 `ayni_submit_intent` 内调用。** `validate(ctx, parameters)` 在 Kernel 所有者线程上同步调用，参数只在调用期间有效，回调不得调用 Kernel、阻塞或执行 I/O。作为兜底，回调运行期间，接受非 const Kernel 的所有者线程函数都返回 `INVALID_STATE` 且不产生效果；否则违反约定的回调可以在校验通过与插入之间关闭会话、取消父目标或改变生命周期，让已经检查过的条件失效。查询不拦截，但同样不属于回调可以依赖的行为，`MA-009` 的策略回调可以据重放需要收紧。C-POLICY-001 据此修订：策略回调在 `step` 内，GoalType 的参数校验在 `ayni_submit_intent` 内。被接受的参数复制进调用方内存块中为每个 Goal 预留的 `goal_parameter_max_bytes` 字节。
9. **模块落位（ADR-0008）。**
   - `foundation`：`kernel_id.h`（ID 布局与分配器）、`transition_stamp.h`、`bounded_array.h`（调用方内存上的定长数组）。
   - `catalog`：`catalog_id`（目录 ID 规则）、`goal_type/goal_type_registry`。
   - `authority`：`principal/principal_registry`（所有者与会话表）。
   - `work`：`agent/agent_table`、`goal/goal_table`（Goal 树、生命周期与版本的不变量）。
   - `kernel`：`intent/`（意图的领域类型、C ABI 解码与语义编排）、`work_api.cpp`（C ABI 入口）、`transition_clock.h`、`abi_checks.h`、`kernel_config.h`（容量、限额与调用方内存的划分）。
   - `kernel/intent/work_model`：`WorkModel` 聚合意图读写的状态，即各工作表、ID 分配器、transition 计数器与回调标记；会话与意图规则是以它为参数的自由函数。规则因此触及不到 Host、恢复与事件队列，`ayni_kernel` 只持有一个 `WorkModel`。

   领域意图类型 `Intent` 与格式校验 `check_form` 在 `kernel/intent/`，C 解码器与将来的网络解码器构造同一类型并共用校验（C-WIRE-014）。

## 后果

- 在 `MA-010` 之前，重启会丢失全部 Goal，实例 ID 也会改变；调用方能从 `commit_seq > durable_seq` 看出这一点，但应用层还不能依赖 Goal 跨重启存在。这是已知缺口，不是可以宣称的能力。
- 终态 Goal 一直占用容量，长期运行的设备会填满 Goal 表并得到 `GOAL_CAPACITY`；回收要等 tombstone 与保留策略。（自 ADR-0025 起，表满时终态 Goal 让位，只有未终结的 Goal 占满表时才得到 `GOAL_CAPACITY`。）
- 查找按 ID 线性扫描（至多 1024 个），取消级联最坏 O(n × depth)。在已配置的上限内可以接受，但尚未测量，随 M1 的 `step` 时长测量一起复核。
- ABI 升至 0.5，0.4 的调用方须重新编译（C-VER-006）。默认配置下 Kernel 内存需求增加，由 `ayni_kernel_memory_requirements` 报告；Native 与 ESP-IDF 组合根使用默认值。
- 状态码之外新增原因码，二者同属 ABI，已发布值不改作他用（C-STATUS-007）。

## 备选方案

- **持久化的 ID 计数器**：每次分配都要先写日志，或预留一段 ID 并在重启后跳过；代际前缀以零写入换来同样的唯一性。否决。
- **每次启动从 1 开始的 64 位计数器、不带代际**：重启后复用旧 ID，违背 C-ID-001。否决。
- **随机 64 位 ID**：碰撞概率不为 0，没有顺序，启动时还需要熵源。否决。
- **本地 API 也先编码再提交（ADR-0009 方案 (b)）**：用户选了 (a)；网络解码器仍构造同一领域类型（C-WIRE-014）。
- **暂停级联到子目标**：见决策 5，需要为每个子目标记录暂停原因。否决。
- **终态 Goal 立即移除以腾出容量**：回执与查询仍可能引用旧 ID，移除需要 tombstone（C-ID-001），还会打乱下标与创建顺序。推迟到 `MA-010`。（ADR-0025 改为表满时才让位，ID 不复用所以不留 tombstone。）
- **先调用 GoalType 校验**：调用方代码会看到随后被拒绝的 Goal，原因码的顺序也取决于调用方代码。否决。
- **等 `MA-010` 后再开放意图**：把两项大改动绑进一个 PR。否决；本 ADR 如实报告“未持久”。
- **会话放在 `work/`**：与 primitives.md 的属主不符，也与 ControlSession 撞名。否决。
- **意图规则作为 `ayni_kernel` 的成员函数**：规则能直接改动 Host、恢复与事件队列，`ayni_kernel` 也随每类意图膨胀。否决，改用 `WorkModel`。
- **允许回调重入并在插入前重新检查**：每条规则都要检查两遍，而且回调可能把状态改到另一个合法却不同的样子。否决，回调期间拒绝修改。

## 验证

- `tests/unit/kernel_id_test.cpp`（`KernelId.*`、`BoundedArray.*`）：ID 带代际且序号从 1 开始；开始之前与用尽之后拒绝分配，不回绕；定长数组的容量、交换删除与零容量。
- `tests/unit/goal_type_registry_test.cpp`（`CatalogId.*`、`GoalTypeRegistry.*`）：目录 ID 的字符与长度；同一 ID 的多个版本；区分未知 ID 与未知版本；重复与容量；校验函数决定参数，没有校验函数时只接受空参数。
- `tests/unit/principal_registry_test.cpp`、`tests/unit/agent_table_test.cpp`：只有一个所有者；会话表的容量与关闭；Agent 注册一次、实例在一个 transition 中取得 ID 与印记、`instance_id` 0 不指向任何实例。
- `tests/unit/goal_table_test.cpp`（`GoalTable.*`）：父目标、owner、深度与子目标数的规则及其报告顺序；零限额只允许顶层目标；容量计入终态 Goal；暂停与恢复只改变目标本身；取消到达每个未终结后代且不波及其他目标；过时版本先于状态检查；版本上限（白盒设置）；表满时插入失败且不改变任何东西。（自 ADR-0025 起，容量与让位见 `GoalTable.terminal_goal_that_ended_first_gives_way`。）
- `tests/unit/kernel_work_test.cpp`（`WorkModel.*`）：经 C ABI 的注册、会话、意图与查询，包括参数与结构体大小、检查顺序、每个原因码与对象引用、拒绝不改变状态、ID 用尽、取消级联、过时版本与版本上限、非 `RUNNING` 时 `NOT_READY`、校验回调不能改动 Kernel、缺省的树限额、`DRAINING` 中只答查询与关闭会话、重启分配新 ID 并丢失未持久的工作且重用其 `commit_seq`，以及八个函数在六个状态下的返回值表。
- `tests/unit/kernel_lifecycle_test.cpp`：`durable_seq` 在恢复中跟随已读日志，进入 `RUNNING` 后是启动批次，追加结果未知时不前进；最大代际与最大 `transition_id` 仍能启动，超出时进入 `RESTRICTED`。
- `tests/unit/kernel_api_test.cpp`：新配置项的范围与默认值、内存需求随各容量增长、树限额不占内存。`tests/contract/abi_layout.c` 固定新结构体的布局，`tests/contract/c_abi_smoke.c` 从 C99 调用新增的八个函数（没有存储端口，所以只到 `CONFIGURING`）。
- Grant、渠道信任级别、持久化与重放尚未实现，相应条款未标为已实现。
