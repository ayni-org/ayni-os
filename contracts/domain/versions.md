# 版本与兼容

- **关联**：计划 §3.1、§3.2「版本」、§4.4；ADR-0004、ADR-0009、ADR-0013；`MA-005`、`MA-010`；E3、E4 步骤 5–6

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
| C-VER-001 | `ayni_create` 只接受 `config->abi_major == AYNI_ABI_MAJOR`；主版本为 0 时还要求 `abi_minor == AYNI_ABI_MINOR`（`struct_size` 未覆盖 `abi_minor` 的调用方按 0.1 处理）；否则返回 `AYNI_UNSUPPORTED_VERSION` 且不写入内存块 | 主版本不同的调用方被接受，按错误布局解读配置；或 0.2 改变了某字段的含义，0.1 的调用方仍被接受 | M0 已实现：`KernelApi.create_rejects_other_abi_major`、`KernelApi.create_rejects_other_abi_minor`、`KernelApi.config_from_abi_0_1_is_unsupported_version`、`KernelApi.failed_create_leaves_memory_untouched` |
| C-VER-002 | 同一 ABI 主版本内只能在结构体末尾追加字段、在表末尾追加函数；已发布字段的类型、顺序与含义不变 | 在 minor 升级中把 `ctx` 移到 `monotonic_now_us` 之后 | M1 已实现：编译期检查 `tests/contract/abi_layout.c` 以 `offsetof` 与 `sizeof` 固定已发布结构体的布局；评审 |
| C-VER-003 | 每个 ABI 结构体以 `uint32_t struct_size` 开头，调用方填写自己所见的 `sizeof`；库只读取 `struct_size` 覆盖的字段，缺少的尾部字段取文档规定的默认值 | 库读取旧调用方结构体之后的内存 | M0 已实现：`KernelApi.create_copies_host_table_before_return`；`KernelApi.config_from_abi_0_1_is_unsupported_version`（`struct_size` 不含 `abi_minor` 时按 0.1 处理）；M1 起另有 `KernelApi.host_table_without_0_3_ports_is_invalid_argument`、`KernelApi.host_table_without_storage_ports_is_accepted`、`KernelApi.uncovered_capacities_take_their_defaults`、`KernelLifecycle.kernel_info_checks_its_output` |
| C-VER-004 | `struct_size` 小于该主版本首个发布布局时返回 `AYNI_INVALID_ARGUMENT`；大于本库所知的布局时返回 `AYNI_UNSUPPORTED_VERSION`（调用方按更新的 minor 编译） | 更长的结构体被静默截断，调用方以为新字段已生效 | M0 已实现：`KernelApi.struct_size_below_first_layout_is_invalid_argument`、`KernelApi.struct_size_from_newer_minor_is_unsupported_version` |
| C-VER-005 | ABI 不跨边界传递 STL 类型、C++ 异常、RTTI 依赖的类型、OS 句柄或第三方 SDK 类型 | 公开函数参数出现 `std::string`、`FILE *` 或 FreeRTOS 任务句柄 | `tests/contract` 头文件以严格 C99 编译；`check_deps` 限制公开头文件只包含 `stddef.h`、`stdint.h` |
| C-VER-006 | 主版本 0 表示 experimental：允许不兼容变更，但每次变更须递增 `AYNI_ABI_MINOR` 并在 PR 中说明受影响的调用方；因此主版本 0 下不同 minor 互不接受（C-VER-001）。主版本 ≥ 1 时 minor 只追加，库接受同一主版本中不高于自身的 minor | 修改已发布函数的语义而版本号不变 | 评审；`AbiVersion.reports_header_version` 保证运行时报告与头文件一致 |
| C-VER-007 | 草案头文件（`include/ayni/draft/`）中的声明不属于已发布 ABI，没有符号；转为实现时整体移入正式头文件，并按 C-VER-002 追加 | 在草案头文件中实现函数并让调用方依赖它 | `check_deps`：正式头文件与 Core 不能包含草案；链接失败即暴露误用 |

## 协议

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-010 | 接收方遇到不同的 `protocol_major` 拒绝消息并返回 `UNSUPPORTED_VERSION`；相同主版本下未知的可选字段忽略，未知的必需字段或动作拒绝 | 新版本增加的必需安全字段被旧接收方忽略并照常执行 | `MA-016`；`wire/` 解析测试（M1） |
| C-VER-011 | 能力调用按能力 ID 与能力版本协商；接收方没有该版本的实现或兼容 resolver 时拒绝，不以最新版本解释旧参数 | v2 解释器执行携带 v1 参数的调用 | E4 步骤 5 |
| C-VER-012 | v0.1 管理 API 与 v0.2 设备互通共用 `domain/` 中的领域类型与校验器，只在信封与传输上不同 | 管理 API 与设备协议各自定义一套 Goal 字段 | ADR-0009 选型后的双向往返测试 |

## 存储

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-020 | 每个持久记录带记录版本；每个快照与日志段带 `storage_schema`。读取到不支持的版本时进入 `RESTRICTED`（只读诊断），不得当作空数据重建 | 无法识别的快照被丢弃，Kernel 以空状态启动 | E4 步骤 6；`MA-010` |
| C-VER-021 | 迁移在切换提交前保留旧格式；迁移中断后可从旧格式重新开始 | 迁移写到一半掉电，新旧格式都不完整 | E4 步骤 6 |
| C-VER-022 | 回滚到旧固件时，若旧固件无法读取当前有效状态，只能进入安全诊断，不得回到过时快照继续执行 | 回滚后旧快照让已结算的预算或已删除的记忆复活 | E4 步骤 6；M4 实机 OTA |
| C-VER-023 | 不持久化 C++ 内存布局；持久格式由 `storage/records.md` 定义，与编译器和 ABI 无关 | 直接 `memcpy` 内存中的对象写入 Flash | 评审；`MA-010` 跨编译器读取测试 |

## 领域语义

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-VER-030 | ActionRecord、步骤检查点与 Binding 保存所用的能力、Scene、GoalType 版本；恢复时只有明确兼容的实现或迁移可以继续处理，缺失时保留记录并暂停 | 能力升级后旧 ActionRecord 被新语义重新执行 | E4 步骤 5；E3 步骤 5 |
| C-VER-031 | 升级清单给出 firmware、`storage_schema`、能力/Scene/GoalType/检查点的读、写、执行兼容矩阵；矩阵与实际行为不一致视为缺陷 | 清单声称兼容，实测旧检查点被拒绝 | E4 步骤 6；M4 |
| C-VER-032 | 幂等参数摘要按契约列出的领域字段规范化计算，不对任何序列化字节取哈希 | 两个语义相同的请求因字段顺序不同得到不同摘要，被判为 `IDEMPOTENCY_CONFLICT` | ADR-0009 选型后的摘要测试；`wire/envelope.md` C-WIRE-012 |
