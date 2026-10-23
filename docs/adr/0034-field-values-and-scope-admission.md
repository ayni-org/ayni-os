# ADR-0034：字段值编解码与 Scope 准入结果复用

- **状态**：实施中；本地验证与独立审查完成后更新结果。
- **日期**：2026-10-23
- **关联**：`MA-006` R2a2；[详细计划](../ma006-implementation.md)；[复查清单](../../reports/core/ma006-a-d-review.md) B1～B5、C1；C-AUTH-017/018、C-WIRE-017、C-VER-003；ADR-0029、0030、0033。

## 背景

结构体域、持久域和原始命令各自实现字段值的部分规则。注册路径重复验证域，Scope 的第二遍处理再次调用可失败校验却忽略结果。两种记录的字段头不同，但整数、单位和 token 载荷相同；整理应共享同一语义，同时保留格式与错误顺序。

## 决策

1. **值语义与载荷归 catalog。** `catalog/capability/field_values.*` 拥有类型/单位/范围、集合规范化、整数转换、载荷读写与相等比较。结构体输入先验证指针、reserved 和 struct_size，允许无序重复成员；字节输入还要求严格递增、无重复和足够字节。两者共用整数、集合和用途规则。域与 raw command 的版本、字段头及尾部检查仍由原模块拥有，禁止混用 reader。二者的字段 ID 插入排序共用 `insert_field_order`，只适配实际已有的两种 ABI 字段类型；各入口保持原调用次序。类型与单位相等共用 `same_value_type`。
2. **ABI 布局检查下移。** 将 `kernel/abi_checks.h` 移至 `catalog/abi_checks.h`，保留同一实现与返回分类，catalog、authority、kernel 共用，不引入向上的依赖或旧位置的转发层。
3. **复用本次准入结果。** `DomainEncoding` 只保存有界字段索引、数量与完整字节数，成功校验才发布；输入必须在同一次所有者线程调用内保持不变。注册入口在既有生命周期检查之前取得该结果，`add_validated()` 只处理重复/容量及复制。独立 `add()` 仍做完整校验。结果不保留到 Kernel 或注册表，不把外部提供的结构视为已校验数据。
4. **Scope 区分约束和实际请求。** 用具名模式替换布尔参数；实际请求仍要求全部字段且每项单值。结构体 Scope 的第一遍保留原有错误次序，第二遍只重建已校验值的规范视图。字节 Scope 的域/父/子有效性与覆盖在同一有界遍历中检查。共享容器/字段头检查，保留 Scope 与原始命令不同的值检查顺序，避免同时存在多种错误时改变分类。
5. **注释和兼容。** 补值、域、注册表和 Scope 的所有权/前置条件说明；恢复排序去重、容量探测与用途位理由。`member_count` 的去重前限额移到其自身注释。原 raw reader 的“以 unsigned bits 保存数字”说明已不适用：它现在保存共用 signed 视图，由唯一 `signed_value` 做无损转换，写入仍是相同的二补码字节。公共 ABI 0.19、记录版本、编码、arena 布局与容量不变。新增对象是本次调用的有界栈局部值，无堆分配、I/O、回调或新 Kernel 字段。
6. **保留测试意图。** 既有字节常量及行为断言保持。`CapabilityRegistry::validate` 的内部输出从字节数升级为包含字节数的准入结果，因此一个既有测试只调整输出类型与 `.size` 访问；原成功/错误状态和成功/失败输出数值断言不变。
7. **本轮授权更新。** 用户 2026-10-23 明确继续完成 R2/R3/R4 及 E/F，撤销 R4 后等待开工的旧停点；三项已选 E/F 产品语义保持。GitHub CI 额度耗尽期间，本轮按 workflow 的本地五配置、静态检查、独立变异/审查及 ESP-IDF 编译取得证据后提 PR 并以 merge commit 合并。仅本轮提交使用 `[skip ci]`，不永久修改 CI 触发规则、不声称远端 CI 已通过。新的产品问题先列选项；用户较长时间未回复时按推荐项推进并记录。

## 后果

- 字段值规则改变时修改一个 catalog 模块；增加新记录头只需在该记录属主适配现有载荷接口，不能绕过完整输入验证。
- `normalized_field_values()`、`write_domain()` 与 `add_validated()` 是 Core 内部的已校验输入路径，调用方必须保持验证和消费之间没有回调、重入或输入修改；不对外开放。
- 为节省有界栈，不缓存两整组展开 Scope。需要第二次读取域时仍复用带边界检查的唯一 reader，避免另写不安全解析器；不再重跑结构体的准入规则并丢弃状态。
- 当前所有恢复的活工作仍以 `LEGACY_UNCONFIRMED` 停下；本次整理不实现 Goal 审批或授权恢复，后续按 R2b→R3→R4→E→F 交付。

## 备选方案

- 保留两份值 codec：会继续让字段语义在域与命令中分叉，拒绝。
- 合并 domain/raw 的完整 reader：两种字段头和语义不同，违反记录契约；只共享载荷。
- 缓存每个 Scope 的全部展开字段：增加与最大字段数相乘的栈占用；本次只保留小的注册准入结果，Scope 复用规范视图构造。
- 一次统一所有错误检查顺序：会改变既有错误分类优先级；只共享规则，由入口保留顺序。

## 验证

原始命令、日志、源码身份、覆盖率及变异清单放在仓库外 `../ayni-ma006-evidence/r2a2/`；本节只填写实际执行结果。初步完整 Debug 检查通过。独立代理 `/root/r2a2_review` 在阅读测试前依据生产 diff/契约冻结 25 项缺陷清单（SHA-256 `2fd1d3cac80969152902c0eb2c3302f31abdb7fee91f752bd906635909bf5d94`）；首轮使用完整 886 项单元入口发现 19 项，6 项存活，25 项均成功编译；02 在出现 29 个命名断言失败后 SIGSEGV，不能声称其运行完全部用例。首轮源码逐项还原，首尾 SHA 清单相同，恢复后的完整单元测试通过。六项补测已完成，详情如下；TSan 与 ESP-IDF 仍在收尾，其他已执行项目逐项记录，不据此标记交付完成。

### 作者逐项可读性自查

1. **删除注释。** 检查 `git diff --unified=0 -- core include/ayni` 的全部语义注释删除行。Coverage 的有效输出约束保留于 `scope.h:14`；ABI 大小分类迁至 `catalog/abi_checks.h:1`；负数转换说明迁至 `field_values.cpp:28`；值视图借用与排序说明在 `field_values.h:16`、`:27`；注册成功才发布计划在 `capability_registry.h:36` / `field_domain.h:50`；token 去重前数量说明在 `include/ayni/authority.h:34`。原 raw reader 以 unsigned bits 保存值的说明因采用共享 signed 视图而失效，删除理由见决策 5；编码未改。
2. **Kernel 新字段。** 检查全部 `core/kernel/` diff，`kernel_state.h` 无变动，无新增 Kernel 字段。`work_api.cpp:244` 的 `DomainEncoding` 是本次调用局部结果，其索引/长度归 catalog，按 `field_domain.h:50` 的输入不变约束立即消费；不保留或复制持久目录状态。
3. **重复公式。** 搜索 `check_struct_size/struct_size != sizeof/normalized_field_values/signed_value/same_type/same_value_type/domain_at/insert_field_order` 及被丢弃的校验调用。ABI 大小规则归 `catalog/abi_checks.h`，值载荷归 `field_values.*`，两种输入字段排序共用 `field_domain.h:22`，类型/单位相等共用 `field_values.cpp:195`，目录槽地址只在 `capability_registry.cpp:74`。两种 framing 保留自身布局；Scope 第二次域读取复用唯一 reader，以避免两整组展开字段占栈，取舍见决策 4/后果。
4. **裸数字。** 检查全部生产增删行及值/域/Scope/命令模块中的常量、索引和位运算。字段头大小在 `field_values.h:14`，载荷宽度在 `field_values.cpp:13`，用途位在 `:225` 只对已验证的枚举移位；各 framing 偏移具名并引用记录布局。搜索本轮 diff 未发现 Kernel ID 的手工位拆分，也没有新增公开状态/原因码。机器检查 34 项历史债务，无新增；不能替代上述人工检查。

### 独立审查

独立代理 `/root/r2a2_review` 先从架构/属主、入口/消费者、失败与恢复路径审查，再逐项检查删除注释、Kernel 新字段、重复公式、裸数字。首次发现四项：当前计划的旧 R4 停点、ADR 索引断表、两处小重复、raw reader 缺函数职责说明，均已修复并获复核；原报告及追加报告保留在上述仓库外证据目录。未发现可证明的行为/字节/兼容性回归。独立源码复核不等于运行测试；最终构建、覆盖率、内存和固件结果另列。

### 独立变异逐项结果

出题者为独立代理 `/root/r2a2_review`，先冻结清单再阅读测试；实现者运行。每项均使用完整单元入口，编译失败不计为检出。首轮 19/25，补测六项 6/6；没有等价豁免或编译失败。02 在断言检出后崩溃，其他项正常返回测试结果。两个轮次均验证源码首尾 SHA 相同，并在还原后分别完整通过 886/886 与 892/892。

| 编号 | 文件与植入变化 | 首轮 | 首轮命名失败（列一项）/新增补测 |
| --- | --- | --- | --- |
| 01 | `core/catalog/abi_checks.h`：C-VER-003：把未来布局错误归为普通参数错误，检查共享大小规则下移后各调用者是否仍保留分类。 | 检出 | `CapabilityApi.malformed_descriptors_do_not_consume_the_only_registration_slot` |
| 02 | `core/catalog/abi_checks.h`：C-VER-003：恰好覆盖字段末尾的已发布 ABI 前缀被误判为未覆盖，检查边界字段读取与默认值。 | 检出，随后 SIGSEGV | `GrantReplay.a_valid_but_expanded_create_scope_cannot_disagree_with_its_exact_command` |
| 03 | `core/catalog/capability/field_values.cpp`：C-AUTH-017/018：闭区间单点也是合法域和实际请求；拒绝相等端点会破坏两种输入共享的语义。 | 检出 | `Scope.actual_request_requires_every_field_exactly_once_and_a_single_value_per_field` |
| 04 | `core/catalog/capability/field_values.cpp`：C-AUTH-017：原始成员数和规范成员数均允许上界 8，检查最大有限集合的接纳。 | 检出 | `CapabilityRegistry.complete_domain_fits_exactly_or_rejects_one_byte_more_without_partial_registration` |
| 05 | `core/catalog/capability/field_values.cpp`：C-AUTH-017：遇到重复 token 过早终止，丢掉重复项之后的新成员；检查无序、重复和后继成员混合输入的完整规范化。 | 存活；补测检出 | `CapabilityRegistry.a_repeated_member_does_not_hide_later_distinct_members` |
| 06 | `core/catalog/capability/field_values.cpp`：C-AUTH-017、records.md finite domain/raw scope：共享 writer 将区间上界写成下界，要求独立持久字节期望能检出，不能仅依赖同一 codec 往返。 | 检出 | `CapabilityRegistry.fields_and_token_members_are_sorted_and_duplicate_members_are_removed` |
| 07 | `core/catalog/capability/field_values.cpp`：C-AUTH-017：负数位模式少减 1，包含 INT64_MIN/-1 的域及 raw canonical 解码改变数值；无实现定义转换或编译错误。 | 检出 | `CapabilityRegistry.fields_and_token_members_are_sorted_and_duplicate_members_are_removed` |
| 08 | `core/catalog/capability/field_values.cpp`：C-AUTH-017、records.md：字节输入必须严格递增；放行相等 token 会把非规范持久输入当合法集合。 | 检出 | `CommandCanonical.enum_and_identifier_require_bounded_nonempty_sorted_tokens_without_units` |
| 09 | `core/catalog/capability/field_values.cpp`：共享 payload reader 的失败输出不变契约：坏 framing 元数据不能清空调用者原值和 consumed。 | 存活；补测检出 | `FieldValues.failed_payload_reads_preserve_both_outputs_at_every_truncation` |
| 10 | `core/catalog/capability/field_values.cpp`：C-AUTH-017：只禁止重复目的地却放过第二个执行目标，检查 struct/domain reader 共用用途唯一性规则。 | 检出 | `CapabilityRegistry.execution_target_and_egress_are_distinct_single_identifier_fields` |
| 11 | `core/catalog/capability/field_domain.cpp`：DomainEncoding 是成功才发布的短期结果；后继字段值非法时不得覆盖调用者原编码计划或泄漏半份校验结果。 | 检出 | `CapabilityRegistry.pure_validation_reports_needed_bytes_without_taking_a_slot_or_publishing_failures` |
| 12 | `core/catalog/capability/field_domain.cpp`：C-AUTH-017：恰好所需容量应成功；改变为 <= 会令容量探测后的精确大小缓冲仍失败。 | 存活；补测检出 | `CapabilityRegistry.standalone_domain_encoder_probes_and_fills_exact_capacity` |
| 13 | `core/catalog/capability/field_domain.cpp`：C-AUTH-017、records.md：write_domain 必须消费检验得到的排序索引，注册输入无序时仍产出按 ID 排序的确定持久字节。 | 检出 | `CapabilityRegistry.fields_and_token_members_are_sorted_and_duplicate_members_are_removed` |
| 14 | `core/catalog/capability/capability_registry.cpp`：行为不变要求：表满时重复 ID/version 仍须按既有顺序返回 INVALID_ARGUMENT，而不是容量错误；同时失败不得变更注册槽。 | 检出 | `CapabilityApi.registration_is_bounded_copies_identity_and_never_commits_or_wakes` |
| 15 | `core/catalog/capability/capability_registry.cpp`：目录唯一 slot 地址公式：第二个及之后 capability 覆盖前一个域，检查多版本/多 capability 的不可变域和槽隔离。 | 检出 | `CapabilityRegistry.filling_the_registry_preserves_every_prior_domain_and_lookup` |
| 16 | `core/kernel/work_api.cpp`：ABI 注册入口既有错误顺序：输入形状/版本在生命周期之前判断；运行态加畸形描述符应仍返回具体输入错误。 | 检出 | `CapabilityApi.an_exact_capability_version_may_coexist_but_registration_freezes_at_start` |
| 17 | `core/catalog/capability/field_domain.cpp`：encode_domain 明确只在 BUFFER_TOO_SMALL 或成功发布 out_size；非法后继字段不得清除调用者哨兵长度。 | 存活；补测检出 | `CapabilityRegistry.standalone_domain_encoder_preserves_outputs_for_invalid_later_fields` |
| 18 | `core/kernel/command/command_canonical.cpp`：records.md raw scope framing：原始字段最后一个 reserved 字节必须为零，不能借共享 payload reader 忽略 framing 属主应拒绝的内容。 | 检出 | `CommandCanonical.complete_outer_identity_still_rejects_malformed_raw_headers_and_integers` |
| 19 | `core/kernel/command/command_canonical.cpp`：records.md：raw 头部 unit_length 与 member_count 相邻却语义不同；检查共享 payload 后 raw 独立 framing 的精确字节。 | 检出 | `CommandCanonical.complete_outer_identity_still_rejects_malformed_raw_headers_and_integers` |
| 20 | `core/kernel/command/command_canonical.cpp`：records.md、C-WIRE-017：完整原请求不允许尾随字节；忽略 exact body end 会接纳额外未解释 payload。 | 检出 | `CommandCanonical.complete_outer_length_cannot_hide_tail_bytes_inside_the_raw_scope` |
| 21 | `core/catalog/capability/field_values.cpp`：C-WIRE-017、records.md Grant/mapping 配对：共享值相等须双端点同时相等；一端相同不能证明 raw 请求展开与已存 Scope 相等。 | 检出 | `CommandCanonical.pairing_expands_omission_against_source_and_rejects_raw_or_stored_damage` |
| 22 | `core/authority/scope/scope.cpp`：C-AUTH-018：typed 实际请求误走约束模式会允许省略字段或范围替代单值；检查入口模式传递。 | 检出 | `Scope.actual_request_requires_every_field_exactly_once_and_a_single_value_per_field` |
| 23 | `core/authority/scope/scope.cpp`：C-AUTH-018：stored allowed + typed 实际请求同样必须完整单值，避免只在 typed/typed 路径正确。 | 存活；补测检出 | `Scope.stored_allowed_scope_requires_a_complete_single_value_request` |
| 24 | `core/authority/scope/scope.cpp`：C-AUTH-017：字节三方比较必须精确匹配字段用途；相同 kind/unit/id/value 但 PARAMETER 与 EXECUTION_TARGET 不同不可互换。 | 检出 | `Scope.encoded_identifier_scopes_cannot_exchange_target_and_egress_usages` |
| 25 | `core/authority/scope/scope.cpp`：既有 parent-before-child 分类错误顺序：两个 operand 同时非法且一方为未来 ABI 大小时，必须返回 parent 的错误而非 child 的错误。 | 存活；补测检出 | `Scope.simultaneous_operand_errors_preserve_parent_before_child_classification` |

### 本地构建与资源（最终两项收尾中）

- GCC 13.3 Debug、Release、ASan/UBSan 和 Clang 18.1.3 Debug 均完整通过 907/907 CTest（892 单元 + 15 契约/工具检查），开启 warnings-as-errors。TSan 仍在执行；固件环境仍在准备，未报告通过。
- 模拟器、83 项 Python 工具测试、依赖/契约/可读性、clang-format 18 和 diff 检查通过。首次全文件格式命令因尚未暂存的头文件重命名而枚举到不存在的旧路径；保留失败日志，正确暂存重命名后同一格式规则通过，没有忽略现存源文件。
- 首轮变异起初为 4 CPU/4 GiB，宿主短时 memory PSI 升高后降至 2 CPU/3 GiB；补测、最终 native 每容器 2 CPU/3 GiB、构建和 CTest 均 2 jobs，容器不额外使用 swap。覆盖率可独立并行时合计 4 CPU/6 GiB，预算前宿主约 13 GiB 可用，保留桌面与其他任务余量；每阶段记录 cgroup 峰值和 OOM 事件。构建配置依次执行。

### 本轮覆盖率与内存

使用 GCC 13 gcov，全 136 个翻译单元（含测试及头文件消费者）按同一路径/行/分支位置的最大计数合并；只排除 throw 分支，不排除防御检查。构建后清空旧 gcda，再完整运行 907/907 CTest；所有 Core cpp 均纳入，运行前后生产/测试 SHA 相同。改动行基线为 main `04fc261`。

| 范围 | 行覆盖 | 分支覆盖 |
| --- | --- | --- |
| Core（含头文件） | 7660/7844，97.65% | 5549/6118，90.70% |
| 本轮属主组件（catalog 字段/ABI、Scope、canonical，含头文件） | 691/693，99.71% | 545/570，95.61% |
| 本轮改动可执行行 | 277/279，99.28% | 255/260，98.08% |
| 新共享校验/读取/排序 helper 组 | 93/93，100% | 96/96，100% |

改动行未覆盖项如实保留：全字节 Scope 的部分 ID/范围不匹配短路、mixed 第二遍 schema 不足的防御返回、standalone domain 的非零容量配空输出入口。未为达标删分支或增加排除规则；Core 行覆盖与本轮安全相关分支均超过项目目标。

严格 C99 探针调用 `ayni_kernel_memory_requirements`：默认配置 **110,792 B**，最小工作容量 **31,616 B**，均对齐 8；与 ADR-0033 相同。该测量是 arena 需求，不是栈峰值、运行时 RSS 或硬件验证。
