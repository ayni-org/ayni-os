# 版本与兼容

- **关联**：计划 §3.1、§3.2「版本」、§4.4；ADR-0004、ADR-0009、ADR-0013、ADR-0026；`MA-005`、`MA-010`；E3、E4 步骤 5–6

Ayni 有三个互相独立的版本域，以及随领域对象走的语义版本。三者不要求数字同步；任何一个变化都不能借另一个的版本号表达。

| 版本域 | 标识 | 适用对象 |
|---|---|---|
| ABI | `abi_major`、`abi_minor`、结构体 `struct_size`、功能位 | `include/ayni/` 中的 C 接口、Host/Provider/策略函数表 |
| 协议 | `protocol_major`、`protocol_minor`、能力版本与功能协商 | 管理 API、设备间消息（`wire/`） |
| 存储 | `storage_schema`、记录版本、迁移版本 | 日志批次、快照、NVS 记录（`storage/`） |
| 领域语义 | 能力、Scene、GoalType、检查点各自的 `version` | 注册目录与 ActionRecord 绑定的语义 |

## ABI

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-001 | `ayni_create` 只接受 `config->abi_major == AYNI_ABI_MAJOR`；主版本为 0 时还要求 `abi_minor == AYNI_ABI_MINOR`；否则返回 `AYNI_UNSUPPORTED_VERSION` 且不写入内存块。`abi_major` 与 `abi_minor` 在每个布局中位置不变，版本先于 `struct_size` 的其余检查；`struct_size` 不足以包含 `abi_minor` 时为 `AYNI_INVALID_ARGUMENT`（ADR-0053） | 主版本不同的调用方被接受，按错误布局解读配置；或 0.2 改变了某字段的含义，0.1 的调用方仍被接受 | M0 已实现：`KernelApi.create_rejects_other_abi_major`、`KernelApi.create_rejects_other_abi_minor`、`KernelApi.config_version_is_judged_before_its_size`、`KernelApi.failed_create_leaves_memory_untouched` |
| C-VER-002 | 同一 ABI 主版本内只能在结构体末尾追加字段、在表末尾追加函数；已发布字段的类型、顺序与含义不变 | 在 minor 升级中把 `ctx` 移到 `monotonic_now_us` 之后 | M1 已实现：编译期检查 `tests/contract/abi_layout.c` 以 `offsetof` 与 `sizeof` 固定已发布结构体的布局；评审 |
| C-VER-003 | 除明确声明为固定布局值类型（如 `ayni_attempt_ref_v1` 和 `ayni_storage_batch_identity_v1`）外，ABI 描述结构体以 `uint32_t struct_size` 开头；固定值类型不可追加字段，改变布局需发布新类型/函数版本（ADR-0040）。调用方填写自己所见的 `sizeof`。主版本 0 下每个结构体只有本次版本的一个布局，库只接受等于它的 `struct_size`，不为旧布局保留默认值或部分写出（ADR-0053）；主版本 ≥ 1 时库只读取 `struct_size` 覆盖的字段，缺少的尾部字段取文档规定的默认值 | 库读取旧调用方结构体之后的内存；或主版本 0 下按旧布局接受短结构体，让本应重编译的调用方静默得到默认值 | M1 已实现（ADR-0053，主版本 0 只收当前布局）：`KernelApi.create_copies_host_table_before_return`、`KernelApi.struct_size_below_this_layout_is_invalid_argument`（配置与 Host 表的旧布局和任意更短尺寸）、`KernelLifecycle.kernel_info_checks_its_output`、`KernelLifecycle.step_rejects_bad_arguments_without_effect`；其余结构体自 0.5 起即只有一个布局：`WorkModel.registration_checks_its_arguments`（含 0.7 与 0.9 的 `ayni_goal_type_v1` 布局）、`WorkStrategy.registration_checks_its_arguments`、`WorkModel.intent_arguments_are_refused_before_anything_else`（含 0.6、0.7 与 0.8 的 `ayni_goal_create_v1` 布局）、`WorkModel.queries_check_their_arguments`（含 0.7 与 0.8 的 `ayni_goal_info_v1` 布局）、`KernelEvents.post_checks_arguments_kind_and_source_before_the_state`、`WorkModel.output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written`、`WorkTask.task_and_instance_bodies_are_checked_before_anything_else`、`WorkTask.task_query_checks_its_arguments`（含 0.9 的 `ayni_task_info_v1` 布局） |
| C-VER-004 | `struct_size` 小于本库接受的布局（主版本 0 为本次版本的布局，主版本 ≥ 1 为该主版本首个发布布局）时返回 `AYNI_INVALID_ARGUMENT`；大于本库所知的布局时返回 `AYNI_UNSUPPORTED_VERSION`（调用方按更新的 minor 编译） | 更长的结构体被静默截断，调用方以为新字段已生效 | M0 已实现：`KernelApi.struct_size_below_this_layout_is_invalid_argument`、`KernelApi.struct_size_from_newer_minor_is_unsupported_version` |
| C-VER-005 | ABI 不跨边界传递 STL 类型、C++ 异常、RTTI 依赖的类型、OS 句柄或第三方 SDK 类型 | 公开函数参数出现 `std::string`、`FILE *` 或 FreeRTOS 任务句柄 | `tests/contract` 头文件以严格 C99 编译；`check_deps` 限制公开头文件只包含 `stddef.h`、`stdint.h` |
| C-VER-006 | 主版本 0 表示 experimental：允许不兼容变更，但每次变更须递增 `AYNI_ABI_MINOR` 并在 PR 中说明受影响的调用方；因此主版本 0 下不同 minor 互不接受（C-VER-001），每个结构体也只有当前布局（C-VER-003）。主版本 ≥ 1 时 minor 只追加，库接受同一主版本中不高于自身的 minor，届时再引入统一的前缀兼容读取（ADR-0053） | 修改已发布函数的语义而版本号不变 | 评审；`AbiVersion.reports_header_version` 保证运行时报告与头文件一致 |
| C-VER-007 | 草案头文件（`include/ayni/draft/`）中的声明不属于已发布 ABI，没有符号；转为实现时整体移入正式头文件，并按 C-VER-002 追加 | 在草案头文件中实现函数并让调用方依赖它 | `check_deps`：正式头文件与 Core 不能包含草案；链接失败即暴露误用 ；ABI 0.29 草案意图使用独立值域，不能别名到已实现的授权命令：`DraftApi.intents_are_unsupported_before_their_bodies_are_read`（ADR-0042） |

## 协议

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-010 | 接收方遇到不同的 `protocol_major` 拒绝消息并返回 `UNSUPPORTED_VERSION`；相同主版本下未知的可选字段忽略，未知的必需字段或动作拒绝 | 新版本增加的必需安全字段被旧接收方忽略并照常执行 | `MA-016`；`wire/` 解析测试（M1） |
| C-VER-011 | 能力调用按能力 ID 与能力版本协商；接收方没有该版本的实现或兼容 resolver 时拒绝，不以最新版本解释旧参数 | v2 解释器执行携带 v1 参数的调用 | E4 步骤 5 |
| C-VER-012 | v0.1 管理 API 与 v0.2 设备互通共用 `domain/` 中的领域类型与校验器，只在信封与传输上不同 | 管理 API 与设备协议各自定义一套 Goal 字段 | ADR-0009 选型后的双向往返测试 |

## 存储

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-020 | 每个持久记录带记录版本；每个快照与日志段带 `storage_schema`。读取到不支持的版本时进入 `RESTRICTED`（只读诊断），不得当作空数据重建 | 无法识别的快照被丢弃，Kernel 以空状态启动 | M1 已实现（日志、快照版本拒绝）：`KernelLifecycle.untrusted_or_unknown_log_restricts_without_writing`、`SnapshotCodec.newer_schema_is_unsupported_and_crc_continues_between_pages`；E4 步骤 6 |
| C-VER-021 | 迁移在切换提交前保留旧格式；迁移中断后可从旧格式重新开始 | 迁移写到一半掉电，新旧格式都不完整 | M1 部分实现（ADR-0038 开发版不提供迁移；旧格式拒绝并保留）：`StorageSchemaBoundary.unsupported_schema_is_still_rejected_after_a_process_restart`；E4 步骤 6 |
| C-VER-022 | 回滚到旧固件时，若旧固件无法读取当前有效状态，只能进入安全诊断，不得回到过时快照继续执行 | 回滚后旧快照丢掉已接受的 Task 调用计数或让已删除的记忆复活 | M1 部分实现（当前 reader 遇不支持的 schema 安全受限；未执行旧固件二进制回滚）：`StorageSchemaBoundary.unsupported_schema_never_installs_a_writer_or_mutates_source`；完整 E4 步骤 6、M4 实机 OTA |
| C-VER-023 | 不持久化 C++ 内存布局；持久格式由 `storage/records.md` 定义，与编译器和 ABI 无关 | 直接 `memcpy` 内存中的对象写入 Flash | M1 已实现（定长小端编解码与分页）：`BatchCodec.round_trip_keeps_identity_and_records`、`WorkSnapshot.paged_round_trip_keeps_the_original_stamps_and_checkpoint`；GCC/Clang 共用固定格式测试；评审 |

## 领域语义

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-030 | ActionRecord、步骤检查点与 Binding 保存所用的能力、Scene、GoalType 版本；恢复时只有明确兼容的实现或迁移可以继续处理，缺失时保留记录并暂停 | 能力升级后旧 ActionRecord 被新语义重新执行 | M1 部分实现（Goal 保存 GoalType ID 与版本，私有检查点保存写入它的策略 ID 与版本；恢复时本次没有该 GoalType 版本或兼容策略的，保留记录并停下，ADR-0026 决策 7）：`WorkRecovery.goal_type_this_boot_lacks_holds_its_goals_and_tasks`、`WorkRecovery.task_its_strategy_cannot_serve_is_held`、`WorkRecovery.held_goal_has_no_due_work`、`GoalRestore.resolve_types_holds_live_goals_whose_type_this_boot_lacks`；ActionRecord、调用步骤与 Binding 的版本随 `MA-008`、`MA-018`；E4 步骤 5；E3 步骤 5 |
| C-VER-031 | 升级清单给出 firmware、`storage_schema`、能力/Scene/GoalType/检查点的读、写、执行兼容矩阵；矩阵与实际行为不一致视为缺陷 | 清单声称兼容，实测旧检查点被拒绝 | E4 步骤 6；M4 |
| C-VER-032 | 幂等参数摘要按契约列出的领域字段规范化计算，不对任何序列化字节取哈希 | 两个语义相同的请求因字段顺序不同得到不同摘要，被判为 `IDEMPOTENCY_CONFLICT` | ADR-0009 选型后的摘要测试；`wire/envelope.md` C-WIRE-012 |

ABI 0.30（ADR-0043）追加 Task 模型计数查询、启动硬上限、kind13 管理命令及 reason70；0.x 调用方重新编译。schema 2 保留：旧 Task 默认事实（0/8/未限制暂停）仍可读；非默认事实使用 REQUIRED type6/v4-v7，管理命令 type9/v6，旧 reader 遇到必需新版本拒绝并保留目录。降配无法容纳原软上限时返回 STATE_EXCEEDS_CAPACITY，不截断或重置。存储兼容规则并非新增恢复验证结果。

ABI 0.31 发布 `calls.h`、Provider/Binding、单次策略调用、所有者查询对账与等待超时的显式恢复，扩展 config/TaskInfo/strategy snapshot/output；0.x 调用方须重新编译并精确匹配次版本。草案 Provider 头只转包含正式头，不保留第二套同名结构。存储 schema 仍为 2，通过 REQUIRED type21/v1、type6/v8–12、type9/v7–8 显式拒绝旧 reader；ABI、schema 与记录版本分别管理。

ABI 0.32（ADR-0046）扩展 Resource 描述/查询并增加 Device/Component 容量、注册和复制查询；配置尾追加字段，旧短配置对应新表关闭。0.x ABI 次版本精确匹配，旧二进制调用方必须重编译。草案 Device 重复定义移除，正式类型仅来自 `resources.h`。没有新增持久记录，storage schema 仍为 2；资源占用/健康不从旧 boot 恢复。

ABI 0.33 增加 GoalType/GoalInfo 的 WorkProfile、后台频率原因及 Core-only SKIPPED。旧短配置仍有效，公共描述/查询按新 ABI 重编译。非默认 Goal 分类使用 REQUIRED type3/v5、snapshot v6；SKIPPED 调用使用 REQUIRED type21/v2，普通记录保持旧版。schema 2 不变，新版本详见 storage/records.md；不把格式规则视作新恢复实验已经执行。

ABI 0.34 发布共享执行许可和本地 gate C 接口，独立库不依赖 Core（ADR-0049）。许可携带完整有界数值包络，草案控制头引用正式类型；未实现的停止账本/目标验证仍为草案。Core 配置和 arena、schema 2 不变，0.x 调用方重编译；本轮未决定设备 wire 格式。

ABI 0.36 起（ADR-0053）主版本 0 的每个结构体只接受本次版本的布局：配置、Host 表、Kernel 信息、step 报告与完成结果不再为 0.1～0.14 的旧布局保留最小尺寸、默认值或部分写出。布局与 `AYNI_ABI_MINOR` 不变；按 0.36 头文件编译的调用方填写 `sizeof`，行为不变，其他 minor 的调用方原本就被拒绝。上文“旧短配置仍有效”“旧短配置对应新表关闭”的说法随之失效。


ABI 0.35 发布 observations/scenes/execution 头文件：有界 BOOL/I64 当前 boot 观察、Scene 精确持久启停、本地 Mock Executor/ExecutionBinding 及执行查询。config 追加四个零默认容量，旧短前缀不自动启用；ProviderRequest 追加许可指针，0.x Host/Provider/SDK 与 gate 消费者统一重编译。Scene 意图编号 16，执行原因 74/75，避免沿用 draft 冲突编号。schema 2 保持；新增 REQUIRED type22/v1 Scene、type9/v9 管理命令、type21/v3 物理调用尾部；旧 reader 明确拒绝并保留目录，本轮无迁移工具。设备 wire 格式仍未决定。

ABI 0.36 把四个执行端端口改为请求，回答经新的 `ayni_executor_post` 从任意非 ISR 线程投递（ADR-0052）；端口签名增加请求身份与完成目标，新增 `ayni_executor_response_v1` 与 `ayni_executor_output_v1`。执行端、Host、SDK 与 Mock 统一重编译；config 字段不变，arena 为执行端多预留回答通道。持久记录与 schema 2 不变。
