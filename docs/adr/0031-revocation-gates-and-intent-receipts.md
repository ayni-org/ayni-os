# ADR-0031：权限与审批（四）：即时撤销、拒绝栅栏与阶段回执

- **状态**：已接受并完成本地验证；五配置各892/892、最终fresh覆盖率/内存与独立审查通过，35项有效变异全部检出（34项原878套件、D30补测后879套件）；D完成/已合并状态随 PR [#28](https://github.com/RobotSe7en/ayni/pull/28) 合并生效。生产 `e4a017a2d4e2d52be10135888de7520fc97590c8` 的 [CI39735323115](https://github.com/RobotSe7en/ayni/actions/runs/39735323115) 八项通过；最终测试/文档head的CI见 [#28 checks](https://github.com/RobotSe7en/ayni/pull/28/checks)，合并前须全绿
- **日期**：2026-10-22
- **关联**：`MA-006` D；ADR-0014、0027～0030；计划 §2.2、§2.4、§2.8、§3.2、§4.2～4.4、§9.1、§14.1；C-AUTH-006、023～027；C-WIRE-018、019；C-STORE-016、017；C-COMMIT-017

## 背景

C 已接入 Grant 创建、期限与受限委派的持久闭环，D 落实用户批准的立即拒绝、失败期间保持拒绝和分阶段回执。若撤销先等待 journal 或介质空间，背压期间旧权限仍会被消费；若自动重载清掉尚未持久的请求，又会把已向调用方接受的拒绝丢掉。

已接受的控制与可恢复领域事实因此需要不同生命周期。沿用现有 command window、caller arena、提交协调器和 writer fence，不增加 Kernel 状态数组、第二套普通命令窗口或授权日志。D 不接真实 Provider/执行许可，也不宣称物理停止。

## 决策

### 1. 接受点与本部分边界

ABI 升为 0.19，0.18 调用方重新编译。`authority.h` 发布 `ayni_grant_revoke_v1`，`GRANT_REVOKE=10` 经统一意图入口提交；`host_authority.h` 发布可信 Host 认证后的 `ayni_get_intent_receipt`。Core 不处理密码或 TLS。

新撤销要求 `RUNNING`、已绑定所有者的 MANAGEMENT Session、非零 command/target/expected_revision。形状、回调/生命周期与 Session 身份先检查；已接受命令身份先于当前目标语义和存储准入。新目标不存在返回 NOT_FOUND，首次 CAS 不匹配返回 REVISION_CONFLICT；可以撤销未持久或已终态的已知 Grant。VOICE 不能接受新撤销。

接受时先确认 Gate 与 pending 容量，再复制完整身份并建立拒绝；无需先有 journal、介质空间或可逐出的 mapping。此时不改变 Grant 持久状态、cursor、window/floor，不分配 transition。空间、维护和窗口均允许时，同次 submit 可以继续应用并返回 APPLIED；否则返回 PENDING。拒绝不安装半个 Gate 或占用命令身份。

D 只处理 Grant 根/祖先链拒绝与后代清理。Goal/Task 的持久暂停、显式恢复与审批随 E/F；真实许可续租、Provider 取消/外发、完整故障矩阵及物理停止仍分别随 `MA-018`、`MA-008`、`MA-022` 及硬件验收。Scene/额度仍明确不支持。

### 2. 统一回执与字段语义

`ayni_intent_result_v1` 追加 phase/effect/applied_boot_epoch；固定 caller-owned `ayni_intent_receipt_v1` 提供同一结果及 principal/command。回执不分配业务对象 ID，错误不发布半份输出。

| 阶段 | 所表示的事实 | effect / object revision | commit_seq / applied_boot_epoch |
|---|---|---|---|
| PENDING | 仅撤销；完整身份已保留，Gate 已生效 | NONE=0；GRANT target ID，revision=0，不预告结果 | 均为 0 |
| APPLIED | 已应用并排入 journal | CHANGED=1、NO_CHANGE=2 或 TARGET_GONE=3 | 接受本次应用的非零 seq 与 boot |
| DURABLE | 本批确认持久，或完整恢复了原 mapping | 原始 effect 与对象结果 | 原始稳定 seq；boot=0 |

CHANGED 的结果为 REVOKED/revision2；NO_CHANGE 为既有终态/revision2，保留其终态 stamp/cursor；TARGET_GONE 只用于已接受请求在自动重载后失去未持久目标，结果 target ID/revision0。它们都有明确持久结果，不构造不存在的 Grant。

普通已接受命令同样返回 APPLIED/DURABLE，旧 v1/v2 mapping 的 effect 默认 CHANGED。DURABLE 只确认根拒绝或显式 no-op，不表示后代清理、Provider 取消或物理停止已完成。

回执查询由 Host 提供已认证 principal，不依赖旧 Session。PENDING 在 FENCED/RECOVERING/RESTRICTED/DRAINING 仍可诊断；部分重放的普通窗口不暴露。身份顺序为 pending、完整 window、floor；未映射且不大于 floor 为 EXPIRED，其余 NOT_FOUND。CONFIGURING/STOPPED 拒绝查询。

### 3. command 与 authority 的状态所有权

`GrantTable` 在 caller arena 拥有容量等于 grant_capacity 的 Gate ID 集合；Gate 与 Grant 槽分开，目标暂时未恢复或消失也不能清掉 Gate。整条祖先链都检查 Gate/REVOKED，因此后代立即拒绝，不等递归清理。`clear_restored()` 只清领域事实，完整 clear 才清 Gate。

`CommandTable` 同时拥有唯一普通结果窗口、canonical stride/scratch，以及 grant_capacity 个专用 `PendingRevoke`。每项保存 principal/binding revision、command、target、原 CAS、固定 16 B canonical 和 queued boot/seq；每目标至多一项。不按每项预留最大 Scope，不保存 caller 指针，也不把这些字段散加到 Kernel。

`CommandMapping` 的 effect 是持久结果；NORMAL/ACCEPTED_REVOKE retention 是易失保留策略。APPLIED mapping 在确认持久并移交 Gate 前 pin 住，移动槽时正文与 retention 一起移动；pin、Gate、pending 和 queued boot/seq 均不写 snapshot/journal。Kernel 只编排两个 owner 的发布与移交。

### 4. 身份、窗口与容量优先级

撤销 canonical 独立固定为 `u32 kind10 / u32 original_expected_revision / u64 target_id`，不含 Session、当前状态或时间。kind9 的原始请求格式1保持不变；两种 auth 命令共用同一结果窗口，FNV 仅作索引，最终比较完整长度和字节。

所有 kind 都先查询 pending；同 ID 不同 kind/内容返回 IDEMPOTENCY_CONFLICT。pending 的 ID 即使低于后来提高的 floor 仍是已接受控制；发布时走受信的 `publish_accepted_revoke`，不重新作为新命令检查 floor。另一条命令撤销同一 pending target 返回 BUSY/REVOCATION_PENDING，不占用被拒命令的 ID。

window 只逐出未 pin 的最小 command ID，并单调提高 floor。全 pin 时普通新命令返回 BUSY/COMMAND_PINNED，不能先改变领域状态；新撤销仍可接受到专用 pending 容量，等待窗口可发布。控制容量不足返回 CAPACITY_EXCEEDED/REVOCATION_CAPACITY，旧控制保持完整。

### 5. 应用、期限与自动重载

step 在普通到期、策略和事件前处理已接受控制；维护冻结期间只接受易失控制和查询，不应用领域 transition。已接受请求再次应用不依赖旧 Session，不重检首次 CAS；按照恢复后的目标事实产生 CHANGED、NO_CHANGE 或 TARGET_GONE。

原 CAS 不限定未来结果。例如 child 清理已在内存中变为 REVOKED/revision2，但未持久，此时显式撤销 expected2 已被接受；失败重载后 child 恢复 ACTIVE/revision1，重提可以产生 CHANGED/result2。canonical 仍保留 expected2，不能再次要求 expected1。原始请求 codec 只检查非零 revision，因此新请求 expected999 仍进入首次 CAS 并返回 REVISION_CONFLICT；D 的已接受 mapping 只能来自 ACTIVE1 或终态2，故写入/读取/restore 的共享校验明确限定原 expected∈{1,2}，三个 effect 均拒绝不可能的999。该边界不把 accepted expected2→CHANGED2 的合法恢复收窄为 expected1。

当前 boot 的 APPLIED 只有 queued boot 匹配且实际 CommitLog durable_seq 覆盖该请求才能确认；不能用跨重载保留的公开 durable floor 证明旧请求持久。隔离旧 writer 并收回借用后，`clear_restored()` 保留 Gate/pending、清窗口并把 queued stamp 降回 PENDING。完整恢复与绑定检查后，仅原 mapping 的完整 canonical 相同才跨重载确认；没有 mapping 则在新 boot 重提。

FAILED/UNKNOWN、重试、FENCED、隔离失败、读取失败、日志损坏、容量不足或绑定失配均不能清掉已接受 Gate/pending、伪造 DURABLE 或恢复输出。全进程断电前未 DURABLE 的控制不承诺跨进程保留；调用方需要断电保证时须观察 DURABLE。DRAINING 拒绝新撤销；仅当 writer 仍有效、`appends()==true` 时排空已接受控制。从 RESTRICTED/失败 reload 进入关闭则不应用新事实，只收回异步借用后安全 STOPPED，Gate/pending 不因该关闭被主动遗忘。

C 的期限规则不变：只持久 duration，deadline 仅内存，恢复的限时 Grant 失效，不因撤销或重载续期。

### 6. 有界后代清理与安全回收

新增持久生命周期 REVOKED=3。显式根记 `revoked_by_root_id=self`，固定应用时的 through Grant ID 上界；after 以 ID 推进，不保存物理槽。每个有界工作项至多处理一个候选，需要时把 ACTIVE 后代置 REVOKED，并与根 progress 同批；已 EXPIRED/REVOKED 的节点不重写终态。新建后代已被 Gate/链检查拒绝，through 不随新对象扩展。

Grant.stamp 保留首次终态顺序，独立 cleanup_stamp 标识进度；推进 cursor 不改变终态回收顺序。只有永久终态已持久、无保留 child/Gate/pending 引用且根清理已完成的叶节点可让位；还须确认最近 cleanup 已持久。按原终态 commit_seq 再按 ID 选，日志固定 replaced ID。E/F 再加入活工作依赖，不提前声称工作恢复已闭合。

### 7. 持久格式、快照与兼容

保持 storage_schema=1，所有授权记录 REQUIRED；格式的精确长度见 `contracts/storage/records.md`。

| 类型/版本 | 内容 |
|---|---|
| 15/v1 GRANT_REVOCATION | target/revision/effect/initial after/through，32 B；与 kind10 的完整结果 mapping 同批 |
| 14/v2 GRANT_STATE | C 的16 B状态再接 revoked_by_root ID，24 B；后代撤销，C14/v1到期继续读 |
| 16/v1 GRANT_REVOCATION_PROGRESS | root/after/through，24 B；与本步后代状态（如有）同批 |
| 9/v3 COMMAND_MAPPING | 原52 B结果 + effect/format/length（u32×3）+ canonical16，80 B；kind9继续v2，ordinary继续v1 |
| 13/v2 snapshot Grant | 创建正文+C的40 B尾+revoker/after/through（u64×3）+cleanup stamp16 B，总尾80 B；兼容读13/v1 |

journal 与 snapshot 共用有上下文的 REQUIRED 分类，不能把 snapshot 专用13/v2当作 journal13/v1。writer 显式为 Grant 写版本2，command 按1/2/3分别写；旧 reader 遇新 REQUIRED 状态只读受限，不回退旧快照。

snapshot 对创建、终态和 cleanup stamp 分别检查快照界限。所有保留后代（含 ACTIVE、EXPIRED 与独立撤销根）的 ID 都不得超过其任一已撤销祖先根的 through；否则拒绝快照，不能让缩短的扫描上限提前结束清理。retained CHANGED target 须为自身根的 REVOKED，终态 seq 等于 mapping seq；retained NO_CHANGE target 须已终态且终态 seq 严格小于 mapping seq；TARGET_GONE target 必须缺席。合法回收的 CHANGED/NO_CHANGE target 可以缺席。非 self revoker 的保留后代 ID 须不大于 root.after，终态 stamp 不大于 root.cleanup_stamp；revoker/after/through 同样检查 snapshot boot 上界。所有 auth mapping 均核对 binding owner、接受序号、合法 Kernel ID/boot；创建 mapping 继续核对完整不可变创建事实与 Scope。窗口/floor 和原接受序号保留，不保存易失控制。

容量上界分别由 records、snapshot、journal 与 arena owner 的具名函数提供；D revoke mapping 最大80 B不超过C创建上界，snapshot 尾与 metadata/Gate/pending 增加的 Native arena 已实测；ESP32 RAM/栈与最坏 step 时长仍待实测。

## 后果

- 立即拒绝和持久事实各有明确 owner/lifecycle；自动重载不能把已接受控制变成新请求。
- pin 可能暂时阻止普通命令；专门控制容量有上界，确认持久后释放，不增加无界队列。
- ABI 0.19 需要调用方重编；storage schema 不变，记录版本独立演进。Scene、额度、完整工作准入与物理停止仍有明确未实现边界。
- Native caller arena 已量测并独立复核；最大 step 扫描成本、栈和 ESP32 资源仍待实测，不用结构公式代替这些验证结果。

## 备选方案

- 等持久后才拒绝：背压期间权限仍可消费，拒绝。
- reload 清掉 pending/Gate 或用旧 seq 对比新 durable floor：会忘记已接受控制或误确认，拒绝。
- 单独建立永久控制结果窗口：重复幂等/淘汰机制，选专门 pending 加唯一结果窗口与 pin。
- 每次 cursor 推进重写 terminal stamp：改变回收次序，选独立 cleanup_stamp。
- 把 DURABLE 解释为后代/执行器停止完成，或承诺 PENDING 跨进程：超出已批准边界，不提供。

## 验证

原始日志、源码清单、审查与量测在仓库外 `../ayni-ma006-evidence/d/`。本节记录 D 自身的已执行事实，不以 C 的五配置、变异或覆盖率替代 D；交付完成/已合并状态随#28合并生效，最终head须先通过自身CI。

- **最终五配置与正常基线。** 新增D30唯一回归后，GCC13.3.0 Debug、Release、ASan+UBSan、TSan和Clang18.1.3 Debug各 build/CTest **892/892**（879单元与13项检查）通过；模拟器退出0，报告 ABI0.19、CONFIGURING→RUNNING→STOPPED。日志及汇总在 `final-validation-round2/{results,summary}.json`，全部工具版本元数据命令亦退出0，mmap由28恢复原32并核对。前轮五配置891项及元数据命令失败记录独立保留于 `final-validation/`，不拿旧891结果替代新增测试后的版本。此前 **126/126** 项定向回归及D30补测正常 **43/43** 定向通过；最新 fresh coverage另完整执行 **892/892**。测试覆盖 API 三阶段、真实 journal/介质空间背压、管理端/CAS、跨 kind/floor 与 pin、冻结快照、UNKNOWN/FAILED、隔离/读取/绑定失败、重载后原映射确认或重提、DRAINING 及进程断电边界；Grant owner 覆盖祖先拒绝、有界 cleanup、终态与清理 stamp 分离及持久安全叶回收。Native 模拟消费者通过不表示真实许可续租或 Provider 取消已接入。
- **持久和兼容回归已通过。** CRC 合法坏输入覆盖根撤销+完整 v3 mapping、后代 state+progress 的同批配对与互斥；错 root/cursor/effect、孤立/重复/截断、REQUIRED、covered history、journal/snapshot 上下文、未来 boot、各 stamp 边界、retained target 历史一致性、原 expected2 重提 CHANGED 及不可能的 accepted999。snapshot 显式写 Grant13/v2，兼容 v1；mapping 保留 v1/v2/v3。部分可达坏输入的短路组合仍未逐项穷尽，见下文覆盖率限制。
- **内存严格 C99 复核。** Native/x86-64 的默认 caller arena **110,792 B**、最小工作配置 **31,616 B**，对齐均 **8 B**；较 C 的106,520/31,072 B 分别 **+4,272/+544 B**。相同 `memory.c` 分别链接首轮、前轮891及补测后最终892 coverage Core静态库，用 `gcc-13 -std=c99 -pedantic-errors -Wall -Wextra -Werror` 编译，结果一致；独立 `GCOV_PREFIX` 在采集后运行，不污染覆盖率。配置、真实命令和结果见 `coverage/round1/`、`coverage/final2/memory.log`。这些数字不证明 ESP32 RAM/栈、最坏 step 时长或硬件验收。
- **最终 fresh 覆盖率已完成。** 清除全部106个旧 gcda 后，GCC/gcov13.3.0 完整执行892项（CTest **48.40 s**），采集 **133 个全部 TU**，包含 Core 头文件消费者，无遗漏 Core cpp；前后 **182 个 core/include/tests 文件 SHA 一致**，变化清单为空。相同路径/行/分支槽按最大计数去重，仅排除 gcov 标记的 throw 分支，不删除防御分支；D 改动以 C 最终格式提交 `d14bf8a9565370e5c370b99919626d1c3d626859` 为固定基线。Core 含头文件行 **7,666/7,862=97.51%**、分支 **5,608/6,192=90.57%**；D owners 含头文件分支 **1,736/1,846=94.04%**；D 实际改动可执行行分支 **1,210/1,342=90.16%**，其中 owner 改动 **858/930=92.26%**。原分子/分母、未命中 slot、全 TU 原始 JSON 和资源见 `coverage/final2/`，首轮857项、旧final891项及定向诊断保留且未混入最终计数。
- **局部覆盖率限制。** 整体 D 改动达到90%目标，不表示每个文件或函数达标：Kernel 接线改动 **352/412=85.44%**，authority_intent 改动 **30/44=68.18%**。Host 开 Session 时核对持久 binding、重载清 Session 后的防御复核，以及单 owner 无回调的 plan→apply→publish 和 Gate/pending 移交等防御路径保留在分母；没有破坏内部配对刷 INTERNAL。另有明确可达、尚未逐项覆盖的坏输入，例如 Boot type15短于8B/零boot target、replay非法同批排列的部分短路组合、snapshot retained target的个别时序/lifecycle组合、无owner运行中 receipt 查询；不能把它们统称不可达。具体源码行、前置条件、区别与剩余 slot 见 `coverage/final2/limits.md`、`d-uncovered.md`、`uncovered-d.json`。这是记录的验证偏差，不是已证明的生产缺陷，也不由整体比例掩盖。
- **审查与修复。** 独立生产审查、Kernel/command 交叉审查及 root 设计自审已留证（`review.md`、`review-final.md`、`kernel-cross-review.md`、`command-cross-review.md`、`root-design-review.md`）。已处理并回看：DURABLE 重试驱动另一 pending 并逐出原结果、watermark 指向未来 boot、无有效 writer 的 DRAINING 误应用、accepted revision999、fresh Host 空间缓存与立即应用资格。root 进一步发现 through 缩小会漏掉保留后代，Grant owner 在 restore/consistent 共享 metadata 校验中限制全部后代及所有已撤销祖先，ACTIVE/EXPIRED/独立撤销根和完整 CRC 快照对照已随正常基线通过，过程见 `root-frontier-followup.md`。删除 Kernel 重复 durable 选择条件后由 CommandTable 单一选择；旧 target/step 注释已同步；command0只排队、只能APPLIED，移除不可达的立即DURABLE分支。上述静态审查不代替各项执行证据。
- **独立变异与真实漏测。** 原36项清单和两阶段来源保留，D24因审查重构移除重复条件而被取代，不计检出、存活或等价；有效35项首轮各跑完整878单元，结果 **34检出、D30存活**。D30删除WorkReplay对同批child state与root progress的root/child绑定，属于真实测试缺口，不是等价。生产维持 `e4a017a` 不变，只新增124行单个回归 `GrantRevokeReplay.a_later_cursor_cannot_repair_a_child_state_paired_with_another_roots_progress`：合法对照中child X/by A与A进度同批，坏历史改配B进度，后一批再推进A，使两种历史最后cursor均对齐。坏历史仍须LOG_CORRUPT，不能依赖最终consistent才拒绝；否则后批会掩盖错误历史。正常879单元已通过，独立按原D30选点完整879单元重跑仅该新场景失败，D30已补测检出；最后恢复原生产、重新编译后正常 **879/879** 通过，16个production target在独立snapshot、外部D树及实际root三方SHA一致，283文件冻结snapshot manifest全匹配，全部184个代码文件三方一致；仅core/README文档随最终事实更新。其余34点保留原878检出记录，不宣称35点全在879套件跑。through修复前10点/844的历史另档，D30原存活/分析/补测/重跑均分别保存，见 `D30-gap-analysis.md`、`mutation-run/summary.json`、`mutation-rerun-1/`；没有编译、信号或超时方式的假检出，独立最终审查没有未解决阻断。
- **前轮工具链记录的独立纠正。** 前轮五配置和模拟器均退出0；runner 最后汇总工具信息时，仅无特权读取 `/proc/sys/vm/mmap_rnd_bits` 被拒绝使该元数据命令退出1。原 `toolchains.log`/`results.json` 保留，独立重采版本命令退出0，记录 GCC13.3.0、Clang18.1.3、CMake3.31.6、Ninja1.11.1、Python3.12.3。TSan结束后以 sudo读取当时值28，并恢复原32、再核对成功；过程在 `final-validation/toolchain-metadata-correction.json`、`toolchains-corrected.log`。不把元数据失败改写为测试失败，也不抹掉该失败日志。
- **PR、CI及未执行边界。** 生产提交 `e4a017a2d4e2d52be10135888de7520fc97590c8`、追加回归及最终文档由 [#28](https://github.com/RobotSe7en/ayni/pull/28) 交付（D完成/已合并状态随该PR合并生效），生产提交的 [CI run39735323115](https://github.com/RobotSe7en/ayni/actions/runs/39735323115) 八项全部通过，含 ESP-IDF6.0.3 的 ESP32-S3 编译和 advisory clang-tidy；编译通过不表示静态分析没有建议项。新增D30回归与最终文档仍须其head自己的CI，不以e4a017的代码CI替代；最终head检查见#28 checks，合并前必须全绿，D完成/已合并状态随#28合并生效。覆盖率任务限2CPU/3GiB、jobs2/CTest2，GTest1.17.0；版本/命令/资源实录在 evidence，不拿并行预算当峰值量测。ESP-IDF 存储/烧录、硬件 RAM/栈/耐久、真实派发/物理停止及完整 E1–E4 尚未验收。

本次执行边界为 C/D 及代码文档收尾，#28合并后本轮停止并等待用户新指示；E/F、`MA-018`、`MA-008` 不自动开始，D 不表示完整 `MA-006` 已完成。
