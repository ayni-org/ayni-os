# ADR-0033：Kernel ID 与 Grant 记录基础规则归属

- **状态**：已接受；R2a1 本地验证与独立代码审查通过，尚未完成整个 R2。PR 与最终 CI 见交付链接。
- **日期**：2026-10-23
- **关联**：`MA-006` R2a1；[详细计划](../ma006-implementation.md)；[A–D 复查](../../reports/core/ma006-a-d-review.md) A5、C3、D4；C-ID-010；ADR-0030、0031、0032；计划 §9.1、§14.1

## 背景

同一个 Kernel ID 的高低位规则散落在 Grant 表、记录、启动与快照恢复中；终态 revision 直接写 2，启动编排又自行解释 Grant 记录的首字段。复查要求将这些知识放回各自属主。本次只做行为不变的基础整理，字段域/Scope 编解码留给 R2a2，Grant 状态和 Kernel 编排留给 R2b。

## 决策

1. **ID 表示归 foundation。** `kernel_id.h` 提供 `id_boot_epoch()` 和 `is_kernel_id()`。前者允许解析零或只有高位的分配器前缀；后者仅验证两个 32 位分量均非零。对象存在性、所有者和允许的 boot 上界仍由领域调用方决定，不增加统一的授权判断。
2. **revision 归 Grant。** `grant_table.h` 定义 ACTIVE 的 `grant_active_revision` 和 EXPIRED/REVOKED 的 `grant_terminal_revision`。表更新、持久记录验证、命令映射和创建命令配对共用常量；清理 stamp 不改变生命周期 revision。Goal、Task、owner binding 的 revision 有各自规则，不套用 Grant 常量。
3. **记录模块拥有字节布局。** `read_grant_record_id()` 只从已分类的 create/revocation body 读取小端首 ID，允许未对齐输入；空指针或不足 8 字节时不修改输出。完整读函数仍验证记录长度、ID 形状和其余内容。BootSequence 负责 create 必须属于当时持久代际、revocation 不得来自未来代际的顺序规则，不在编排层解释偏移。
4. **固定长度可追溯。** owner binding、Grant、规范请求头引用 `contracts/storage/records.md` 的对应条目；command v2/v3 增加的字段按 `sizeof(uint32_t)` 表达并说明字段。保留原有独立字节数组测试，不改成使用生产常量生成期望值。`2U/3U * sizeof(uint32_t)` 按 D4 的明确建议保留为 Named values 规则的局部例外：注释逐项列出固定字段，直接显示字段数量，避免另添两层名称；长度本身仍有具名常量。PR How 同样列明此例外。
5. **兼容与资源。** 不改变公共 ABI 0.19、记录版本、编码字节、字段布局、容量或对象状态。不新增 Kernel 字段、堆分配、I/O 或异步接口。无需持久迁移；回退本 PR 可直接读取相同日志和快照。新辅助接口限于 Core 内部。

## 后果

- 修改 ID 表示时，位操作只有 foundation 一个属主；Grant 的未来版本若增加生命周期转换，必须同时重新审查其 revision 契约及消费者。
- 启动恢复使用前缀读取而非完整解析，保留既有检查顺序，避免编排层复制记录规则或执行两次完整域/范围校验。这个函数不能被当作记录有效性的证明。
- R1 检查基线删除 15 项 ID 位操作和 1 项已达到注释比例要求的 command canonical 模块，剩余 35 项按后续计划处理；不把机器通过当成可读性完成证明。

## 备选方案

- 为每种 ID 建立包装类：本次没有需要改变类型契约的消费者，迁移代价超过消除重复的需要；保留明确的两项纯函数。
- BootSequence 直接调用完整 Grant reader：会提前执行领域校验并在 replay 再做一次；采用记录属主提供的窄前缀接口。
- 一次完成全部 R2a：会同时改 ID、记录、字段语义和 Scope 算法；按稳定规则的属主拆成两份独立验证的 PR。

## 验证

原始日志与独立植入缺陷清单保存在仓库外 `../ayni-ma006-evidence/r2a1/`。以下均为本轮实际运行结果；仓库保留测试与本节逐项摘要，原始本机日志不在仓库中。

- **环境与资源。** Native/x86-64，`ayni-m0-tools:latest`：GCC 13.3.0、Clang/clang-format 18.1.3、CMake 3.31.6、Ninja 1.11.1、Python 3.12.3。首轮变异使用 2 个构建任务、容器 2 CPU/3 GiB；20 次构建合计 240.827 秒、完整单测合计 586.259 秒。用户询问耗时后重新核对宿主约 17 GiB 可用、CPU/内存压力为 0，按实测峰值调整为 4 个构建/测试任务、单容器 4 CPU/4 GiB，无额外 swap；独立覆盖率与最终构建并行时总上限 8 CPU/8 GiB。最终 Native、TSan、覆盖率容器峰值分别为 1,409,257,472 / 1,004,470,272 / 1,165,754,368 B，均无 OOM；期间宿主仍约 16 GiB 可用，压力为 0。预算和原始采样见 `resource-budget-adjustment.json` 及各 results.json。
- **构建与运行。** GCC Debug、Release、ASan+UBSan、TSan、Clang Debug 均开启 warnings-as-errors，各 **901/901 CTest** 通过（886 个单元测试及 15 项其他检查）。模拟器 ABI 0.19，CONFIGURING→RUNNING→STOPPED，退出 0。TSan 按 workflow 临时用 `mmap_rnd_bits=28`，结束后恢复原值 32 并核对；本轮未再重复已知的 32 启动失败尝试。
- **工具检查。** Python 工具测试 **83/83**；依赖、契约、可读性、clang-format 18、diff 检查通过。可读性基线 51→35，只删除已消除项，没有提高上限或增加豁免。纯文档收尾后再运行契约与 diff 检查。
- **独立审查。** `r2a1_review` 复核固定副本及三项补测，确认各缺陷由实际授权/恢复断言检出，未发现行为回归；唯一交付说明 R2a1-R01 以决策 4 和 PR How 的局部例外解决。以下四项分别保留作者与独立审查证据。

交付前设计自查已核对生产 diff 与调用链：基础表示留在 foundation，Grant 生命周期留在 authority，Kernel 保留先后顺序和跨表一致性规则；未增加抽象层或运行时状态。初轮独立只读审查会话 `r2a1_review` 对固定副本的 181 个 Core/include/test 文件核对 SHA 后审查，未发现行为或兼容性回归；它不构成已运行测试的证据。

| 项目 | 作者自查 | 独立审查 |
|---|---|---|
| 删除注释 | `kernel_id.h:15–19` 保留原最大 boot 说明；`command_canonical.h:19–20` 保留 revoke 独立格式说明，均补充出处 | 完整生产 diff 未发现仍成立但丢失的说明；启动顺序和 reload/CAS 注释保留 |
| Kernel 新字段 | 检查全部 `core/kernel/` diff，`kernel_state.h` 无改动；`boot_sequence.cpp:540` 仅增加一次调用内的解析输出 | 无 Kernel/BootSequence/replay/snapshot 成员变化，无新增复制状态或 bool 组合 |
| 重复公式 | 检查 ID 位移/低位强转及 Grant revision 引用：表示仅在 `kernel_id.h`，revision 仅在 `grant_table.h` 定义；create 与 revoke 的 boot 关系各自保留 | 独立搜索 Core/include 的位运算、强转和常量消费者；其余低位计算属于既有 commit marker 例外 |
| 裸数字 | 位移与 revision 均具名；四组记录头补布局出处；`command_records.h:19,22` 的两项固定字段数量例外见决策 4 | 唯一说明事项 R2a1-R01 是此局部例外；不影响布局，ADR 与 PR How 显式记录 |

### 覆盖率与内存

本轮清除旧 gcda 后运行完整 CTest，**901/901** 通过，再采集全部 **133 个 gcno 翻译单元**，含测试对 Core 头文件的实例化；按同文件/行/分支位置取最大命中次数，仅排除 throw 分支，无 Core `.cpp` 遗漏。采集前后源码 SHA 一致。原始数据为 `coverage/final/summary.json`、`uncovered-core.json` 与对应 gcov JSON。

| 范围 | 行 | 分支 |
|---|---|---|
| Core 含头文件 | 7683/7879 = 97.51% | 5606/6192 = 90.54% |
| 本次修改的可执行行（相对 `a491600`） | 74/74 = 100% | 155/170 = 91.18% |
| Grant/command 及 authority intent/receipt 模块（含头文件） | 1807/1834 = 98.53% | 1732/1842 = 94.03% |
| 新增 ID 与前缀读取辅助函数 | 9/9 = 100% | 8/8 = 100% |

局部偏差明确保留：本次 Kernel 编排改动单独计为 **30/38 = 78.95%**，其中 BootSequence 为 7/10、WorkReplay 为 14/18，snapshot 为 9/10。未命中项集中在前缀错误与不同 epoch 上界的短路组合；完整读取、记录分类和跨表校验会提前拒绝部分非法组合，但不能据此把所有缺口都称为不可达。本次保留这些检查，并保留在分母中，不用总体数字替代局部数字。实际三项漏检已由独立变异补测关闭；其余未命中行/分支按原始清单保留为后续状态编排整理的验证输入。Grant/command 改动分支为 121/128 = 94.53%。

严格 C99 独立调用 `ayni_kernel_memory_requirements()`，用此前相同的默认配置与最小工作表配置测得 **110,792 B / 31,616 B**，对齐均为 **8 B**，与 ADR-0031 的测量相同。测量程序和编译/链接参数见仓库外 `memory.c`、`coverage/final/memory-commands.json`，计数写到独立 GCOV_PREFIX，未污染测试覆盖率。这里测的是 Native caller arena，不代表硬件 RAM、栈或耐久实验。

### 独立植入缺陷

独立会话 `r2a1_review` 先按生产 diff/契约出题，再查看测试。首次以 883 个单元测试逐项运行，20 项均编译成功，17 项检出、3 项存活，无等价项。之后只增加三个回归场景，原选点 10/13/14 各由对应场景检出；正常源码定向 3/3 通过，前后源码 SHA 一致。补测采用定向优先、未检出再跑全量的策略，本次三项均定向检出，未触发全量回退。最终完整套件另行运行。

| 缺陷 | 文件（省略 `core/`）与修改 | 首次结果 | 命名断言（存活项为补测后） |
|---|---|---|---|
| 01 | `foundation/kernel_id.h`：允许零 boot | 检出 | `KernelId.shape_rejects_either_zero_component_and_accepts_both_word_boundaries` |
| 02 | `foundation/kernel_id.h`：允许零 seq | 检出 | `KernelId.shape_rejects_either_zero_component_and_accepts_both_word_boundaries` |
| 03 | `foundation/kernel_id.h`：只保留 epoch 低 16 位 | 检出 | `KernelId.epoch_decode_accepts_allocated_ids_and_allocator_prefixes` |
| 04 | `foundation/kernel_id.h`：耗尽后 boot 返回 0 | 检出 | `KernelId.allocator_runs_out_instead_of_wrapping` |
| 05 | `authority/grant/grant_records.cpp`：拒绝恰好 8 字节前缀 | 检出 | `GrantRecords.id_prefix_decode_is_unaligned_and_does_not_validate_the_remaining_body` |
| 06 | `authority/grant/grant_records.cpp`：前缀只读低 32 位 | 检出 | `GrantRecords.id_prefix_decode_is_unaligned_and_does_not_validate_the_remaining_body` |
| 07 | `authority/grant/grant_records.cpp`：失败时清零输出 | 检出 | `GrantRecords.id_prefix_failure_preserves_output_for_null_and_every_truncation` |
| 08 | `authority/grant/grant_table.h`：终态 revision 从 2 改为 3 | 检出 | `GrantRevokeRecords.root_state_and_progress_match_independent_little_endian_literals` |
| 09 | `authority/grant/grant_table.cpp`：ACTIVE 恢复要求终态 revision | 检出 | `GrantTable.snapshot_lifecycle_and_both_original_stamps_are_checked_independently` |
| 10 | `authority/grant/grant_table.cpp`：只拒绝较早 boot 的限时 Grant | 存活；补测检出 | `GrantTable.finite_grants_authorize_only_their_own_boot` |
| 11 | `authority/grant/grant_table.cpp`：允许限时 child 来自较晚 boot | 检出 | `GrantTable.restore_rejects_expansion_or_issuer_schema_depth_and_lifetime_disagreements` |
| 12 | `authority/grant/grant_table.cpp`：到期仍保留 ACTIVE revision | 检出 | `GrantApi.arrival_at_deadline_denies_immediately_before_step_records_the_terminal_state` |
| 13 | `kernel/boot/boot_sequence.cpp`：允许旧 boot 的新建记录 | 存活；补测检出 | `GrantReplay.an_earlier_boot_create_is_corrupt_even_when_mapping_and_create_agree` |
| 14 | `kernel/boot/boot_sequence.cpp`：撤销也强制同 boot | 存活；补测检出 | `GrantRevokeReplay.revocation_of_a_prior_boot_grant_replays_with_its_original_result` |
| 15 | `kernel/work_replay.cpp`：允许 cleanup frontier 来自下一 boot | 检出 | `GrantRevokeReplay.snapshot_cleanup_watermarks_cannot_name_a_future_boot` |
| 16 | `kernel/work_snapshot.cpp`：删除 snapshot 结果 ID 形状检查 | 检出 | `GrantReplay.snapshot_auth_result_requires_nonzero_epoch_counter_and_no_future_boot` |
| 17 | `kernel/work_snapshot.cpp`：允许 snapshot 结果来自下一 boot | 检出 | `GrantReplay.snapshot_auth_result_requires_nonzero_epoch_counter_and_no_future_boot` |
| 18 | `kernel/command/command_table.cpp`：接受原 CAS 3，排除 CAS 2 | 检出 | `CommandRevoke.impossible_accepted_revision_is_rejected_atomically_for_every_effect` |
| 19 | `authority/grant/grant_records.cpp`：NO_CHANGE 错用 ACTIVE revision | 检出 | `GrantRevokeRecords.root_outcomes_validate_revision_frontier_and_known_effect_atomically` |
| 20 | `kernel/command/command_records.h`：v3 固定字段从 3 个减少为 2 个 | 检出 | `CommandRevoke.accepted_revision_two_can_change_a_restored_active_target_after_reload` |

本轮未做本地 ESP32 编译、烧录、硬件 RAM/栈或耐久验证；CI 的固件编译与实际合并状态以交付链接为准。R2a2/R2b/R3/R4 及 E/F 尚未实现，A1 行为缺陷仍留在独立的 R3。

交付：[PR #31](https://github.com/RobotSe7en/ayni/pull/31) 与其 [CI checks](https://github.com/RobotSe7en/ayni/pull/31/checks)。本地验证对应生产代码提交 `4d809e053dd1f852ed4e6d26575755385c51296d`；之后仅补交付链接。最终 CI 必须对应最新文档提交，实际结果与合并状态以这些链接为准。
