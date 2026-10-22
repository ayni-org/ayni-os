# MA-006 A–D 复查与整理任务

- **关联**：#25–#28；ADR-0028～0031；`docs/workflow.md` 第 2 节；`AGENTS.md`；`reports/core/m1-progress.md`
- **基线**：main `643f586`（#28 合并后）。下文的行号都在这个提交上，改动后会移动，以内容为准。
- **来源**：2026-10-22 用户请 Claude 检查 Codex 的 #25–#28。四个 PR 各由一个只读审查会话对照 `docs/workflow.md`（#24 版）检查，再由两个会话在 `643f586` 上逐条核对。没有构建或运行测试。
- **用户要求（2026-10-22）**：由 Codex 先解决本文列出的问题，并修改 `AGENTS.md`，避免再犯，尤其是代码难读的问题。

## 1. 结论与交付顺序

A–D 的状态归属做对了：`struct ayni_kernel` 没有新字段，状态都在 `PrincipalRegistry`、`CapabilityRegistry`、`GrantTable`、`CommandTable` 里。问题集中在可读性：新模块注释极少（`grant_table.cpp` 901 行只有 7 行注释），同一条规则在多处各写一遍，revision 与 ID 位运算写成裸数字，几组字段拼成没写出来的状态，`step()` 越来越长；另有一个行为问题（A1），状态文档堆满了 CI 编号与过期的条件句。四个 PR 的 Review 小节都只用一句话带过 workflow 第 10 步的四项。

按下面的顺序交付，每个 PR 都按 `docs/workflow.md` 第 2 节做（独立审查、四项逐项结论；PR 2、PR 3 属于核心逻辑，要做植入缺陷测试）：

1. **PR 1 约束**：按第 5 节改 `AGENTS.md`（含第 3.5 节 DOC3），同步 `docs/workflow.md` 第 2 节第 8、10 步；按第 6 节加 `tools/check_readability.py` 并接入 CI，基线如实列出现状。
2. **PR 2 整理代码**：第 3 节 A2–D6 中的代码问题，只改结构、注释与测试，运行行为不变（做法照 #23）。生产代码（`core/`、`include/`）改动超过约 1,500 行时拆成两个 PR，例如 B/C 的编解码与校验一个、C/D 的 Grant 表与重放状态一个。
3. **PR 3 行为修复**：第 4 节的 A1，先写失败的测试再改。
4. **PR 4 文档**：第 3.5 节的 DOC1、DOC2，重写交接状态与计划的“当前状态”，删掉过期的条件句，补契约验证列。

做完后停下，等用户决定何时开始 `MA-006` E。

## 2. 每一项怎么处理

第 3 节每一项在 PR 正文里逐项写出处理：改了（commit 与 file:line），或不改的理由。“建议改法”只是建议，可以换更好的做法，但要在 PR 里写明。整理中发现的同类问题照样处理并补进 PR 正文。

## 3. 要整理的问题

### 3.1 A（#25，可信所有者绑定）

| 编号 | 问题 | 位置（643f586） | 建议改法 |
|---|---|---|---|
| A1 | **行为问题**，见第 4 节。因 `LEGACY_UNCONFIRMED` 停下的活 Goal 查询显示 PAUSED，但 `GoalTable::check` 与 `allows` 仍按存储的生命周期判断，所以 `GOAL_PAUSE` 被接受、`GOAL_RESUME` 返回 `INVALID_STATE`。Task 用有效生命周期，停下时拒绝恢复。没有测试对停下的 Goal 发暂停或恢复 | `core/kernel/work_api.cpp:136-140`；`core/work/goal/goal_table.cpp:273, 452-466`；对照 `core/work/task/task_table.h:201-208`、`task_table.cpp:280-287` | `allows` 对暂停与恢复也要求 `hold_reason != AYNI_REASON_LEGACY_UNCONFIRMED`，与 Task 同一规则 |
| A2 | 删掉了仍然成立的注释 “The types apply() restores, so the two lists change together.”；`knows()` 改名 `classify_header` 后两份类型清单仍并列 | `core/kernel/work_replay.cpp:55`；清单在 `:69-109` 与 `:128-157` | 恢复这句注释 |
| A2b | 头文件说只有 OWNER_BINDING 缺 REQUIRED 时返回损坏，C/D 之后 Grant 记录与 v2+ 命令记录也是 | `core/kernel/work_replay.h:45-46`；`work_replay.cpp:59-68` | 改成 “an authority record” 并列出 |
| A2c | `static_assert` 没覆盖 `classify_header` 已接受的新版本（撤销 v3、GRANT_STATE v2、快照 v2、revocation、progress） | `core/kernel/work_replay.cpp:160-167` | 补齐并更新提示信息 |
| A3 | 同一个会话检查里主体查了三次；`Session.binding_revision` 复制了 `Principal.binding_revision`，C/D 又在多处读它 | 查找：`core/kernel/work_api.cpp:339`、`core/kernel/intent/work_model.cpp:55, 64`；字段：`core/authority/principal/principal_registry.h:75`；读取：`core/kernel/intent/authority_intent.cpp:20, 49, 114, 122`、`intent_rules.cpp:310` | 把 `:339` 已找到的 `const Principal &` 传给 `kernel::open_session`，去掉另两次查找；字段保留并注释为“打开会话时主体的 revision” |
| A4 | if / else-if 两个分支检查同一个受限条件 | `core/kernel/kernel_lifecycle.cpp:346-353` | 先判失配：`if (mismatch) finish(); else if (RECOVERING) discard();`，行为不变 |
| A5 | 记录头文件开头没有引用 `contracts/storage/records.md` | `core/authority/principal/owner_binding_records.h:1-2`；`core/authority/grant/grant_records.h:1` | 照 `agent_records.h:1` 写出处 |

### 3.2 B（#26，Capability 取值域与范围）

| 编号 | 问题 | 位置（643f586） | 建议改法 |
|---|---|---|---|
| B1 | 取值域合法性（单位、min ≤ max、1–8 个成员、执行目标与外发目的地各至多一个）在结构体输入与字节输入各写一遍；B 首轮漏掉的 5 个缺陷有 4 个在后一份 | 结构体：`core/catalog/capability/field_domain.cpp:53-98, 162-170`；字节：`DomainReader::next`，`:221-291`（主要 `:235-264`） | 两条路径共用小的校验函数（范围与单位、标识符形状、用途位） |
| B2 | 同一个 `struct_size` 检查写了 4 次；`kernel/abi_checks.h` 已有，但分层规则不许下层引用 kernel | `field_domain.cpp:26-30`；`core/catalog/capability/capability_registry.cpp:23-25`；`core/authority/scope/scope.cpp:43-45, 52-55`；已有：`core/kernel/abi_checks.h:22-31`；分层：`tools/architecture.json:3-10` | 把 `check_struct_size` 移到 catalog 根部一个 kernel、authority、catalog 都能引用的头文件 |
| B3 | 重跑检查并丢掉返回状态，C 还删掉了唯一一处解释；注册时每个描述解析 4 遍（`work_api` 校验一遍且 `domain_size` 没用，`capability_registry` 再校验一遍，编码又分两遍） | 丢状态：`scope.cpp:157, 158, 187, 219, 245, 246`、`field_domain.cpp:191`；四遍：`core/kernel/work_api.cpp:244-249`、`capability_registry.cpp:40, 54-55`、`field_domain.cpp:151-181, 188-194` | 已检查过的地方写“已由上面的 inspect() 检查”；把校验得到的 `domain_size` 传给不再重复校验的 `add()` |
| B4 | 注释极少：`field_domain.cpp` 293 行 3 行，`scope.cpp` 263 行 2 行，`capability_registry.cpp` 83 行 0 行；没有说明有序插入与去重、为什么 `BUFFER_TOO_SMALL` 算合法、用途位掩码 | 插入与去重：`field_domain.cpp:75-92, 171-179`；`BUFFER_TOO_SMALL`：`capability_registry.cpp:29-35`；用途位：`field_domain.cpp:166-170, 241-245, 287`、`field_domain.h:60` | 每处一行注释，说明原因与出处；类和公开成员补注释（第 5 节） |
| B5 | `bool actual` 模式参数穿过 4 个函数，调用处是裸 `true`/`false`；`member_count` 的规则写在 `reserved` 的注释里；步长公式写两次 | 参数：`scope.cpp:77, 98, 140-141, 227-228`，调用：`:143, 157, 172, 187, 197, 202, 255, 260`；`include/ayni/authority.h:35`；步长：`capability_registry.cpp:16-18, 53, 80` | 换成 `enum class Values { constraint, actual }`；把“去重前 1–8 个”的说明移到 `member_count`；加私有 `domain_at(index)` |
| B6 | 契约 C-AUTH-017/018 的验证列没有列测试名（C-AUTH-019～027 都列了）；`limits.md` 只指向 ADR-0029，下一行没有验证状态 | `contracts/domain/authority_and_trust.md:35-36`；`contracts/domain/limits.md:34-35` | 引用 `capability_registry_test.cpp`、`scope_test.cpp`、`kernel_capability_test.cpp` 中的测试，重新生成 `traceability.md` |

### 3.3 C（#27，Grant 与委派）

| 编号 | 问题 | 位置（643f586） | 建议改法 |
|---|---|---|---|
| C1 | 命令的规范编码大段复制了 B 的取值域编解码（常量、`check_size`、写入与读取）；`same_values` 在同一文件写了两次；`scope_form` 又重复了 `inspect()` 的检查 | `core/kernel/command/command_canonical.cpp:18-35, 38-42, 117-150, 154-259, 269-285, 287-304`；原份：`core/catalog/capability/field_domain.cpp:14-24, 26-30, 117-140, 199-291`；`core/authority/scope/scope.cpp:42-66` 对照 `command_canonical.cpp:76-111` | 由 `field_domain.h` 导出字段常量、值的写入与读取（按用途字节区分）；`RawField` 换成 `FieldValues`，只留一个 `same_values`；两处 `check_size` 改用 `kernel/abi_checks.h:22` |
| C2 | 同一条规则多处各写一遍：主体可用 4 处；父授权收窄 3 种写法；期限规则 3 处；Scene/Quota 拒绝 2 处，存储标志掩码 2 处；`known_flags` 2 处；管理端会话检查 2 处；创建时父链走 2–3 遍 | 主体：`core/kernel/intent/authority_intent.cpp:41-43`、`core/kernel/intent/work_model.cpp:38-40`、`core/kernel/work_replay.cpp:510-512`、`core/kernel/intent/intent_rules.cpp:131-132`；收窄：`core/authority/grant/grant_table.cpp:355-369, 449-461, 640-649`；期限：`grant_table.cpp:408-414`、`core/authority/grant/grant_records.cpp:43-49`、`command_canonical.cpp:50-51`；Scene/Quota：`authority_intent.cpp:25-27`、`grant_table.cpp:421-423`，掩码：`grant_records.cpp:48`、`core/kernel/command/command_table.cpp:18`；`known_flags`：`grant_table.cpp:13-14`、`command_canonical.cpp:32-33`；会话：`authority_intent.cpp:15-23, 111-117`；父链：`grant_table.cpp:445`，再经 `authority_intent.cpp:63-72` → `work_model.cpp:31, 35-49` | 各写一次：`AgentTable::usable()`、返回原因的收窄判断、`valid_lifetime()`、`grant_table.h` 中一个标志常量、一个管理端会话检查；创建路径只走一次父链 |
| C3 | 裸数字：Grant revision 1/2（ACTIVE/终态）直接写字面值；从 ID 取 boot epoch 的 `>> 32U` 在 6 个文件写了 16 次，`kernel_id.h` 没有针对任意 ID 的函数；`known_id`/`kernel_id` 写了三份；启动序列绕过编解码从记录偏移 0 读 grant_id（D 又为类型 15 加了一份）；记录长度没有引用 `records.md` | revision：`grant_table.cpp:64-65, 237, 323, 520, 567, 705, 712, 745`、`grant_records.cpp:155, 241, 247`、`command_table.cpp:16, 25, 34, 36`、`work_replay.cpp:434`；`>> 32U`：`core/kernel/boot/boot_sequence.cpp:543, 548, 549`、`core/kernel/work_snapshot.cpp:695`、`work_replay.cpp:453-457`、`grant_table.cpp:34, 39, 366, 493, 647`、`grant_records.cpp:27`；三份：`grant_table.cpp:33-35`、`grant_records.cpp:26-28`、`work_snapshot.cpp:695-698`；偏移：`boot_sequence.cpp:538-551`；长度：`core/authority/grant/grant_records.h:1, 22-28`、`core/kernel/command/command_canonical.h:13, 19` | revision 用命名常量；在 `core/foundation/kernel_id.h` 加 `id_boot_epoch(id)` 与 `is_kernel_id(id)` 并全部改用；在 `grant_records` 加 `record_grant_id()`；长度注明 `contracts/storage/records.md` |
| C4 | 隐式状态：`Grant.revision` 只是生命周期的镜像，5 处一起设置却没写这个不变量；`deadline_us == 0` 兼作“恢复来的、不可用”，头文件注释没提；`CommandTable` 的暂存候选现在是 5 个字段（D 加了 `staged_kind_`），以长度 0 作标志，8 处重置 | `core/authority/grant/grant_table.h:58-59, 62`；设置处：`grant_table.cpp:258-259, 322-323, 520-521, 566-567, 744-745`；`deadline_us`：`grant_table.cpp:37-40, 368-369, 693`；暂存：`core/kernel/command/command_table.h:170-174`，重置：`command_table.cpp:106, 175, 196, 239, 308`、`core/kernel/command/pending_revoke.cpp:12, 179, 240` | 在 `grant_table.h:58` 写明不变量；`deadline_us` 的特殊值命名并注释；暂存候选收成一个 `Staged` 结构，加一个“有没有”的明确状态，一处重置 |
| C5 | 注释极少：`grant_table.cpp` 901 行 7 行，`grant_records.cpp` 278 行 2 行，`authority_intent.cpp` 175 行 1 行，`grant_table.h` 189 行 14 行；`GrantTable` 40 个公开声明里 32 个没有注释；没有说明槽位让位规则（ADR-0030 决策 7）、生命周期规则、快照与日志恢复为何分两路 | 让位：`grant_table.cpp:467-485`；生命周期：`:57-69, 711-713`；两路恢复：`:164-180, 657-672` | 按第 5 节补：每个类与公开成员一行，非显然分支写原因并引用 ADR/契约 |
| C6 | 长函数：`check_create` 83 行，`restore` 80 行，`WorkSnapshotWriter::next_record` 117 行 | `grant_table.cpp:406-488, 622-701`；`core/kernel/work_snapshot.cpp:186-302` | 抽出有名字的步骤 |
| C7 | 测试注释没有引用契约条目或 ADR 决策（`docs/workflow.md` 第 2 节第 5 步） | `tests/unit/kernel_grant_test.cpp`（16 个）、`grant_table_test.cpp`（29 个）、`grant_replay_test.cpp`（43 个）；D 的 `grant_revoke_test.cpp`、`kernel_revoke_test.cpp` 也没有；`command_revoke_test.cpp`、`pending_revoke_test.cpp` 只在第 1 行写了 ADR-0031 | 每个测试一行注释写它验证的条目 |

### 3.4 D（#28，撤销与分阶段回执）

| 编号 | 问题 | 位置（643f586） | 建议改法 |
|---|---|---|---|
| D1 | `WorkReplay` 的 7 个待配对字段没有注释，以 0 表示“没有”，合起来是没写出来的状态机；各 `restore_*` 各查其中一部分，`discard()` 逐个清零；`complete()` 的注释仍只说 goal 与 task | `core/kernel/work_replay.h:57-58, 112-118`；检查：`core/kernel/work_replay.cpp:299-300, 322-325, 341-342, 360-364, 390, 403-405`；清除：`:370-371, 399, 411-413`；`discard()`：`:588-594` | 收成一个带注释的 `BatchPairing` 结构，写明合法组合；`discard()` 用 `= {}` |
| D2 | PENDING 回执构造了两次，APPLIED 也在回执里重建，`submit` 再用回执覆盖结果；DURABLE 在回执里单独判断，没有 `next_durable_pending` 的 boot 检查 | `core/kernel/intent/authority_intent.cpp:89-106`；`core/kernel/intent/intent_receipt.cpp:27-30, 35-40, 38`；`core/kernel/work_api.cpp:102-114`；`core/kernel/command/pending_revoke.cpp:252-253` | 回执调用 `pending_revoke_outcome`，只补 DURABLE，并注释为什么不需要 boot 检查（或补上检查并加测试） |
| D3 | `grant_table.cpp` 里撤销根判断 8 处（`work_journal`、`work_snapshot` 另有 3 处）；“游标之后下一个 ID”扫描 3 处；子授权撤销更新 2 处；祖先遍历 5 种，循环边界有的用 `<` 有的用 `<=` | 根：`core/authority/grant/grant_table.cpp:76, 273, 724-725, 769-770, 786, 795-796, 837, 880`、`core/kernel/work_journal.cpp:90, 134`、`core/kernel/work_snapshot.cpp:92-93`；扫描：`grant_table.cpp:286-292, 737-742, 845-850`；更新：`:321-325, 743-747`；遍历：`:144-155, 164-180, 378-402, 762-778`、`core/kernel/intent/work_model.cpp:35-49` | `is_revocation_root()`、`next_id_after()`、`revoke_by()` 与一个祖先迭代器；统一并注释循环边界 |
| D4 | 终态 revision 2 写死；`command_record_size + 12U` 与 `+ 8U` 没有出处 | `grant_table.cpp:237, 323, 745`；`core/kernel/command/command_records.h:18, 20`（定义见 `contracts/storage/records.md:103, 105`） | 命名常量；写成 `3U * sizeof(uint32_t)` 一类并注明 records.md 的条目 |
| D5 | 新原因码 56–59 没有说明，也没有 “Since 0.19”（前面每个原因码都有）；`settle_revocations`、`revocation_ready` 没有注释；D 给 `work_replay.cpp` 加的 143 行没有注释 | `include/ayni/ayni.h:856-859`；`core/kernel/kernel_state.h:107-108`；`core/kernel/work_replay.cpp` | 补说明与 “Since 0.19”，补方法注释 |
| D6 | 撤销查找暂存的数据被 `insert_exact` 拒绝，`staged_kind_` 只为挡住它而存在；`ayni_kernel::step()` 146 行（#27 时 118 行）；`settle_revocations(bool)` 用布尔参数选两种算法 | `core/kernel/command/pending_revoke.cpp:50-55`、`core/kernel/command/command_table.cpp:157-159`、`command_table.h:174`；`core/kernel/kernel_lifecycle.cpp:63-208`（撤销循环 `:92-115`，报告 `:173-207`）；`kernel_lifecycle.cpp:517-545` | 先用测试确认无用后删掉 `pending_revoke.cpp:50-55` 与 `staged_kind_`；从 `step()` 抽出撤销循环与报告；拆成两个有名字的函数 |

### 3.5 文档

| 编号 | 问题 | 位置（643f586） | 建议改法 |
|---|---|---|---|
| DOC1 | 合并前的条件句没删（“随 #28 合并生效”“合并前须全绿”“待合并”） | `docs/adr/0031-revocation-gates-and-intent-receipts.md:3, 110, 120, 122`；`reports/core/m1-progress.md:16, 47, 57, 61, 63-65, 84, 97`；`contracts/storage/records.md:5, 115`；`docs/ayni_v1.0.md:9, 127, 135, 1153`；`docs/adr/README.md:45`；`core/README.md:18, 24`；`README.md:7, 62`；ADR-0028:17 仍写“C/D 分别实施及验证” | 改成已合并的事实，证据指向 ADR |
| DOC2 | `m1-progress.md` 的交接状态（第 59-71 行）成了一整段 CI 运行编号、SHA 与覆盖率，并混着 `MA-010` 的旧内容（“随 #21 合并生效”、分支重置、已实现功能清单、571/571、24/24、40/40、584/584、602/602）；计划第 9 行的“当前状态”同样堆着 CI 编号、SHA、覆盖率与内存 | `reports/core/m1-progress.md:59-71`；`docs/ayni_v1.0.md:9` | 按第 5 节重写：做完了什么、下一步、等用户的事；数字移到 ADR 的“验证” |
| DOC3 | `AGENTS.md` 第 7 行仍写 ABI 0.15、ADR-0027 | `AGENTS.md:7` | 见第 5 节 |

## 4. 行为问题

- **A1（修复，单独一个 PR）：** 见第 3 节 A1。先写一个失败的测试：对因 `LEGACY_UNCONFIRMED` 停下的 Goal 依次发 `GOAL_PAUSE`、`GOAL_RESUME`，断言结果与查询显示的状态一致，并符合 ADR-0028 第 4 节“`GOAL_RESUME` 不能解除授权原因”。再改实现。这是让实现符合 ADR-0028 已有的规则，按 workflow 第 2 节第 7 步记录（PR 正文说明，需要时在 ADR-0028 相关句末加括注），不改写已接受的 ADR 正文。
- **重启后所有活工作都停下（不在本轮修，交给 E/F）：** `core/kernel/work_replay.cpp` 的 `hold_unconfirmed_work()` 在每次恢复时把全部活的 Goal/Task 以 `LEGACY_UNCONFIRMED` 停下，包括 A 之后由已绑定所有者新建的工作，因为 A–D 里还没有 Goal 带授权元数据。这符合 ADR-0028 第 4 节的字面规则，但该 ADR 的“后果”只写“从旧版恢复的活工作会停下”。本轮只做两件事：在 `reports/core/m1-progress.md` 的交接状态里写明“当前任何重启都会停下全部活工作，直到 F”；把“经批准的新工作重启后照常继续”写进 E/F 的完成定义（ADR-0032/0033 与计划 §9.1 的 `MA-006` 行）。

## 5. 要写进 AGENTS.md 的约束

现有 `AGENTS.md` 第 42、46、86 行已经要求注释解释不变量、不要用布尔量拼状态机、不要重复校验，但 #21 和 #25–#28 都没做到。原因是这些规则只有原则、没有可检查的标准，审查也只写一句“已检查”。所以新约束要具体、能检查，并要求 PR 拿出证据。

按下面的草案改 `AGENTS.md`（英文，与文件一致；措辞可调整，标准不放松），并同步 `docs/workflow.md` 第 2 节第 8、10 步引用它：

```markdown
## Readability Requirements

These rules apply to `core/` and `include/ayni/`. They are concrete because the
general rules above did not prevent hard-to-read code (#21; #25–#28, see
`reports/core/ma006-a-d-review.md`). A PR that cannot meet one names the
exception and the reason in its How section.

- **Comments.** Every class, struct and public member in a header states what
  it owns or decides. A field states its unit, its "none" value, and the
  invariant that ties it to other fields. A function longer than about 20 lines
  starts with a comment on what it decides and why. A branch that is not
  obvious from the code (a fallback, a status treated as success, a reuse or
  eviction choice, an ordering rule, a check skipped because an earlier one
  holds) says why and cites the contract item or ADR decision. A module (header
  plus source) under `core/` keeps comment lines at or above 5% of its lines
  and close to its neighbours (the median in `core/` is about 12%).
- **Deleted comments.** A deleted comment that is still true is restored where
  the code now is; otherwise the PR says why it no longer holds.
- **One rule, one place.** A validation, computation, encoding or tree walk is
  written once and called. Struct input and byte input share one validator; a
  record or wire layout has one reader and one writer; a call does not
  re-validate what it already validated. Before adding a check, search for an
  existing one (`git grep`) and reuse or move it.
- **Named values.** No bare literal other than 0 or 1 used as a count or index.
  Lifecycle and revision values, record and field offsets and sizes, bit shifts
  and masks, limits and thresholds are named constants; record layouts cite
  `contracts/storage/records.md`. The parts of a Kernel ID are read only
  through `core/foundation/kernel_id.h`.
- **Explicit state.** Fields that are set and cleared together become one
  struct or optional value with a comment on its valid combinations, or an
  enum. Do not use 0 or an empty size as an unstated "none" flag. Do not copy a
  value another module owns unless the PR states why and what keeps the copies
  equal. A boolean parameter does not select between two algorithms; write two
  functions. Do not re-run a check and discard its status.
- **Function size.** Split functions over 80 lines into named steps (SLAP,
  above). A dispatcher such as `ayni_kernel::step()` delegates rather than
  grows.
- **Public codes.** Every new status or reason code in `include/ayni/` has a
  one-line meaning and a "Since 0.N" note.
- **Tests.** Test comments name the contract item or ADR decision they verify.

## Review and Status Evidence

- A readability review states a result for each item in
  `docs/workflow.md` section 2 step 10, with file:line evidence, or with what
  was searched when nothing was found. One sentence covering every item is not
  a result. The author's self-check (step 8) reports the same items separately.
- Another agent or session writes the planted-defect list. The PR names who
  wrote it and gives the list (file, change, and the test that caught it) with
  the first-run result before any added test; results after added tests are
  reported separately.
- When one part's production changes (`core/`, `include/`) exceed about 1,500
  changed lines, split it into PRs that each build and pass, for example
  records and codec, then the table, then the API.
- Status documents stay short. The 交接状态 section of
  `reports/core/m1-progress.md` and the 当前状态 paragraph of
  `docs/ayni_v1.0.md` say what is done, what is next and what waits on the
  user. CI run IDs, SHAs, coverage percentages and test counts belong in the
  ADR's 验证 section and the PR body. Merge-conditional wording ("待合并",
  "随 #N 合并生效", "合并前须全绿") is removed once the PR merges.
- A claim marked verified must be checkable from the repository, the PR or CI.
  Evidence kept outside the repository is labelled as such.
```

同时更新 `AGENTS.md` 第 7 行：它仍写着 ABI 0.15、ADR-0027，main 已是 ABI 0.19、ADR-0031。改成不随版本过期的写法（例如“当前 ABI 与验证状态见 `reports/core/m1-progress.md`”）。

## 6. 机器检查

能由机器查的不再只靠自觉。新增 `tools/check_readability.py`（与 `check_deps.py`、`check_contracts.py` 同样风格），并加进 CI 的“依赖、契约与格式检查”一项：

- `core/` 中超过 80 行的函数；
- `core/` 中注释行低于 5% 的模块（头文件加源文件合计，150 行以上的才算）；
- `core/foundation/kernel_id.h` 之外对 Kernel ID 的 `>> 32U` 或 `& 0xFFFFFFFF`；
- `include/ayni/` 中前面没有注释的 `AYNI_REASON_*` 与状态码定义。

现有违例写进脚本旁的基线清单，只让新增违例失败；整理（第 1 节的 PR 2）消掉的违例同时从基线删掉。脚本放在 PR 1，基线先如实列出现状。

## 7. 完成标准

- 第 3 节每一项都按第 2 节写出处理。
- 整理 PR 只改结构、注释与测试：现有测试不改（必须改的写明原因），`ayni_kernel_memory_requirements` 两组数字不变或写明变化的原因。
- 每个 PR 的 Review 小节对四项各有结论；植入缺陷按第 5 节的写法报告。
- `tools/check_readability.py` 在 main 上通过，基线比 643f586 时少。
- 做完后在 `reports/core/m1-progress.md` 的交接状态里写一句结论，停下等用户决定何时开始 E。
