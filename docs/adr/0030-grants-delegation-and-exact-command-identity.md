# ADR-0030：权限与审批（三）：Grant、受限委派与确切命令身份

- **状态**：已接受并实现；前轮五配置各 779/779、内存与覆盖率量测通过，独立审查发现已修复并回看；新增回归后的最终验证及 50 项独立变异收尾中，尚未 PR/CI/合并
- **日期**：2026-10-22
- **关联**：计划 §2.2、§2.3、§2.5、§2.8、§3.2、§4.2～4.4、§9.1、§14.1；`MA-006` C；ADR-0014、0017、0026～0029；C-AUTH-003、007、009、019～023；C-WIRE-004、012、017；C-STORE-004、007、014、015；E2 F0、F1、F9、F11，E4 步骤 6

## 背景

A 已建立可信 Host 会话与持久 owner/revision，B 提供 Capability 的有限类型域、规范 Scope 和覆盖机制。C 是第一次开放可持久 Grant 的接口：如果只加入创建而把确切命令身份、期限、恢复或安全回收留到后续，同一请求在响应丢失后可能产生多份权限，重启也可能让旧限时授权重新有效。

现有命令映射只保存摘要，不能用于授权管理的确切幂等。目录每 boot 重新注册；同 ID/版本仍可能因错误配置而改变完整声明，单纯验证旧 scope 仍在新域之内不足以证明旧授权语义未变。C 必须保存足够事实并拒绝无法证明的有效性，同时沿用现有 caller arena、提交协调器、journal 和快照，不另建授权日志或 Kernel 全局状态数组。

## 决策

### 1. 本部分范围与可信入口

- ABI 升为 0.18，0.17 调用方须重新编译。`authority.h` 发布 `ayni_grant_create_v1`、Grant/字段的固定 typed 查询输出和 `ayni_get_grant`、`ayni_get_grant_field`。统一意图入口实施 `GRANT_CREATE`，可选父引用表示委派。
- 创建根或子 Grant 都要求已绑定 owner 的 `MANAGEMENT` Session、非零 `command_id`；不能依据 subject 字段把调用者认作该实例。`VOICE` 不得生成新权限。根创建 `expected_revision=0`；委派校验父 Grant 当前 revision。
- 已知 `REQUIRES_SCENE`、`REQUIRES_QUOTA` flags 返回 `UNSUPPORTED/GRANT_UNSUPPORTED`；未知位属于形状错误。不接受后忽略，不把额度缺省变成无限授权。Scene 仍不支持，额度随 `MA-018`。
- C 只实施 Grant 创建、查询、受限委派、期限、安全回收及其确切命令/持久恢复闭环。D 才有撤销 Gate、待决命令与阶段回执；E 才有 Goal 专属授权/批准；F 接完整工作准入。C 不宣称已有 Provider、真实派发复核或物理停止证据。

### 2. 固定 typed 查询与所有权

`ayni_get_grant` 返回固定元数据，`ayni_get_grant_field` 按 index 复制一个完整字段，所有输出由 caller 拥有，不返回 Core 内部指针。查询可见创建事实、范围、生命周期/revision、原始提交、期限模式与有效性原因；lifecycle是将被持久的领域事实，可能仍在等待落盘，不是已持久确认。须结合commit_seq与Kernel durable_seq判断，effective_reason也会给出GRANT_NOT_DURABLE；输出只在成功时整体发布。内部 canonical bytes 不因此成为公开 wire 协议。

易失到期时刻只描述本 boot：永久 Grant 及从旧 boot 恢复的限时 Grant不提供可继续使用的 deadline，以期限模式和有效性原因区分。创建返回接受 transition 的引用/序号，不能据此当作已持久可用；统一 `PENDING/APPLIED/DURABLE` receipt 随 D，C 不新增另一套回执。

### 3. Grant 不可原地扩权与完整父链

- Grant 绑定 subject instance、owner principal/revision、Capability exact ID/version、范围、期限、再委派权限及父引用。续期或扩权新建 ID；ID 不复用。一个 child scope 必须被一条完整 parent scope 覆盖，各字段合取，不拼接其他 Grant。
- 委派沿全部父链检查；任一节点过期、暂时不兼容、未持久或不可委派均不能产生可用后代。未知 parent、revision 冲突、范围越界、深度超限分别保留分类原因；父尚未持久返回 `BUSY/GRANT_NOT_DURABLE`。
- depth 按边数，根为 0；默认最多 4 条委派边，可配置 1–16。校验链中的 ID、深度和重复，不只相信保存的 depth。scope、期限与再委派权限只能收窄。
- 父为当前 boot 限时则 child 也须当前 boot 限时，运行时绝对 deadline 不得晚于父；永久父可产生永久或限时 child。时间加法溢出明确拒绝，不能饱和成永不过期。

### 4. 创建时完整域与展开范围分别保存

每 Grant 自有 metadata 和两个 `authority_scope_max_bytes` 字节 stride：创建时完整 `source_domain`，以及 B 规范化、补全省略字段的 `scope`。两者均复制，不保存 caller 指针或当前目录 index。展开 scope 必须在 source domain 内。

恢复可不依赖当前目录读取合法持久事实，然后按 exact Capability ID/version 比较当前完整注册域的 length+bytes。字段类型、单位、用途或完整取值域任一不同均以 `GRANT_CATALOG_CHANGED` 暂停使用；缺失目录/主体同样保留并拒绝。只检查 scope 仍为新域子集会漏掉 `[0,10]` 变为 `[0,20]` 而授权仍为 `[1,2]` 的漂移，因此不足。兼容性 hold 不写成终态，不导出快照，也不使对象可被回收；父 hold 同样限制全部后代。

默认两份 payload 的公式为 `32×256×2=16,384 B`，极限为 `1024×4096×2=8,388,608 B`，尚未包含 metadata、命令、journal。此处是表示成本，不是内存实测或 ESP32 可行性证据。后续可用实测决定是否值得增加共享 schema 存储；本次优先清楚的所有权和恢复契约。

### 5. 当前 boot 期限与不含单调时间的持久记录

期限分 `PERMANENT`（duration 为 0）和 `CURRENT_BOOT`（duration 非零），限时从接受时开始，不能因等待持久而续期。创建 boot 从 Grant ID 高 32 位取得；只有当前 boot 内存保存和比较 `deadline_us`。journal 和 snapshot 只保存 duration，不保存任何 monotonic deadline（C-STORE-014 无例外）。

重启或自动重载恢复的全部限时 Grant 立即失效，不从 recovery now 重建 deadline。replay 检查合法期限/父引用/同 boot/必要 duration 收窄关系，不能复原旧绝对时刻来宣称有效。当前 boot 消费/委派每次按 now 检查，即使到期 transition 因存储背压尚未提交，也立即拒绝。

普通 step 将到期 Grant 作为有界工作项，先按 deadline、再按 ID 结算；首个启动领域 transition 结算旧 boot 仍为 ACTIVE 的限时 Grant。最大批次公式按各记录上界保守加总，计入有界 Grant state 数及完整 auth mapping，不在恢复 finish 中伪造持久终态；该预算不表示全部记录可以同时发生，work-only 压力场景按实际编码边界验证。快照写持久领域视图，不导出易失 deadline 或兼容性 hold。

### 6. 原始规范命令与展开授权范围分开

授权命令必须保存完整有界 canonical 请求；FNV仅作快速索引，最终比较length+bytes；当前v2 mapping还校验result为新Grant/revision1，拒绝把已知但不支持的Scene/Quota flags作为已接受命令持久化。命令身份包括 kind、expected_revision、cap ID/version、subject、parent、flags、lifetime、原 duration 和原始 typed scope；不包括 Session、当前时间、计算后的 deadline 或新 Grant ID。

原始 scope 只做独立形状检查、字段排序和集合排序去重，保留字段省略，不查询当前目录、不补全域、不先验证 subject 或 parent 当前有效。这样，目录缺失/变化、主体停止、父过期乃至对象已安全回收后，同一已映射命令仍先返回首次结果。省略与显式全域是不同原始请求；token 顺序和重复不改变身份。

`CommandTable` 拥有逐行逐槽 canonical 字节和一个有界 scratch，全部从 caller arena 分配；不把 scratch 加到 Kernel，也不在栈放最大字节数组。先 measure 原始请求并定位已有 mapping，长度不同可直接冲突；等长再编码并逐字节比较。未映射请求才检查当前目录、授权、容量与存储准入。拒绝不保存 mapping/floor，不分配 ID/stamp。旧映射对象已回收时仍可返回旧引用，后续 query 可以 `NOT_FOUND`。

### 7. 终态持久后安全让位

只回收永久失效、终态记录已持久且无保留 child 的叶节点；后续 D/E 引入的 Gate/活工作引用须进入同一安全检查。当前到期但未记录终态、终态未持久都不是回收候选；仅目录/主体hold不足以令ACTIVE Grant可回收，已经持久EXPIRED的永久失效事实不因临时hold改变。

按终态 `last_stamp.commit_seq` 再按 Grant ID 选最早，不用创建顺序冒充终态顺序。同一启动批次的终态用 ID 破同序。日志固定被替代 ID，replay 验证并照此替换，不依据本次配置重选。无安全候选返回 `CAPACITY_EXCEEDED/GRANT_CAPACITY`，不接受新命令。保留 child 会阻止 parent 回收；已保留的命令结果不要求旧 Grant 永久占槽，重试不能借回收重新创建。

### 8. 持久格式、同批完整性与旧 reader

沿用 `storage_schema=1`，以 REQUIRED `GRANT_CREATE`、`GRANT_STATE` 和 `COMMAND_MAPPING` v2 演进；具体字节由 `contracts/storage/records.md` 与 codec 同步固定。C 的初态为 ACTIVE/revision 1，到期为 EXPIRED/revision 2；不提前接受撤销状态。

Grant 创建事实与完整 canonical mapping/结果同批；journal不能只恢复其中一半，并核对mapping的owner/capability/subject/parent/期限/再委派标志、原expected_revision与创建结果；原始scope必须在保存source_domain下展开后恰等保存scope。该配对使用逐字段流比较，没有另一份最大scope字节缓冲。snapshot 保留 retained Grant 的创建与最后 stamp、完整域/范围及父关系，mapping 保留各自原始接受序号与 floor；快照中原创建 mapping 可能已由正常窗口淘汰，不要求所有历史命令永久保留。

每个 transition 至多一份命令结果，replay 复用 CommandTable 的本批新增 seq 拒绝第二份 mapping 或第二组 Grant/mapping；create 与 mapping 不得跨批配对。snapshot 中 auth mapping 必须与 OwnerBinding 主体一致、原接受 seq 严格晚于 binding，结果 KernelID 的 epoch/counter 非零且不超 snapshot boot。若结果 Grant 仍保留，核对原 created seq、不可变创建事实和原始 scope 的确切展开；已回收对象允许不存在并继续返回旧引用。child 的原 expected_revision 按创建时父 revision1 校验，不拿 parent 后来到期的当前 revision2 误拒合法快照。普通 v1 外主窗口继续按原保留规则恢复。

旧 ordinary `COMMAND_MAPPING` v1 继续读取，auth 必须有完整 canonical；不能把空 canonical、未知 REQUIRED 版本或超当前字节配置的 auth 状态降为 hash 或静默跳过。REQUIRED 分类在 replay、Boot discovery（含已被快照覆盖的历史）和 snapshot 共用。布局、最大记录、transition、snapshot、存储缓冲公式由各 owner 的具名函数提供，query/create 同源。

### 9. 模块归属

`authority/grant` 拥有对象、期限/父链/回收不变量及 Grant codec；`catalog/capability` 拥有完整域；`authority/scope` 拥有覆盖。`kernel/command` 拥有原始 canonical、确切比较、命令窗口与 scratch；Kernel 只编排 Session、实例/目录解析、ID、stamp、同批 journal 和恢复。authority 不依赖 kernel/work，不新增授权事务框架，不向 `ayni_kernel` 散加状态布尔量。

## 后果

- ABI 0.18 有明确兼容成本：调用方重新编译，新配置尾字段遵循省略默认/显式零拒绝；新增原因码 41–55 保留每项所属状态，不借一个通用错误遮蔽分类。
- RAM 增加完整源域、scope、命令正文/scratch以及更大 journal；必须量测默认和最小工作容量，不以公式替代实测。
- 目录同版本改变会安全暂停，恢复原声明后可以重新证明兼容；C 不自动迁移或扩权。限时 Grant 重启失效比 Goal 计时的既有重锚规则更保守，两者不能混用。
- C 创建接口不会使旧 Goal/Task 自动获得权限，`LEGACY_UNCONFIRMED`、原进度与其他 hold 仍保留，审批和显式恢复随 E/F。

## 备选方案

- 只保存 FNV：碰撞不能作为授权命令身份，拒绝。
- 先查当前目录再规范幂等输入：旧有效命令在升级后会丢失首次结果，拒绝。
- 只保存展开 scope：无法检测同版本完整域漂移，拒绝；采用 source domain+scope 双 stride，成本显式量测。
- 持久 monotonic deadline 或重启重新计算完整 duration：前者违反 C-STORE-014，后者会复活限时权限，均拒绝。
- 暂时 hold 或创建最早的 Grant 直接让位：不能证明永久失效与终态次序，拒绝。
- 把撤销、Goal 审批和真实调用合入 C：跨越六 PR 边界，继续由 D～F 及 `MA-018`/`MA-008`/`MA-022` 分别验收。

## 验证

原始脚本、日志、源码清单、审查和变异结果在仓库外 `../ayni-ma006-evidence/c/`。下列明确区分已完成的一轮验证与独立变异补测后的待闭合验证；A/B 的结果不计作 C 的执行证据。

- **工具链与资源。** `ayni-m0-tools:latest`，GCC/gcov 13.3.0、Clang/clang-format 18.1.3、CMake 3.31.6、Ninja 1.11.1、Python 3.12.3。构建、测试、独立变异及 coverage 前均检查宿主内存、swap、其他任务；宿主约 17–18 GiB 可用，无持续换页。各验证容器限 2 CPU/3 GiB，memory-swap 同为 3 GiB，`CMAKE_BUILD_PARALLEL_LEVEL=2`、CTest parallel=2；最多 build/mutation/coverage 合计 6 CPU/9 GiB，留出宿主余量。TSan 沿开发环境要求使用 `vm.mmap_rnd_bits=28`；不通过改测试规避 sanitizer。
- **前轮完整五配置已执行。** GCC `native-debug`、`native-release`、`native-asan`（ASan+UBSan）、`native-tsan`，及 Clang 18 Debug 均 build/CTest **779/779** 通过，启用 warnings-as-errors，固定 GoogleTest 1.17.0 由 `/gtest` 离线提供；无 sanitizer 报告。`final-validation/results.json` 及各配置日志记录完整命令、返回码、时长和资源，模拟器退出 0。早期 746 项日志继续保留；随后独立变异又补回归并修正失败测试 helper，当前源码的最终五配置须重跑，779 项不能冒充新增回归已在全部配置执行。
- **内存已执行并独立复核。** Native/x86-64 严格 C99 程序 `memory.c` 调用 `ayni_kernel_memory_requirements`：默认配置（`struct_size=12`）**106,520 B**；最小工作容量（含 Grant/Capability 容量 1、Scope 64 B、命令容量 1，完整字段见程序）**31,072 B**，对齐均 **8 B**（`memory.log`）。较 B 的 67,544/29,060 B 分别 **+38,976/+2,012 B**；包含 Grant 双 stride、命令正文/scratch及更大 journal，不把表示公式当作各项独立实测。另一 coverage 产物严格 C99 编译同一 probe，结果一致（`coverage/memory-independent.log`）；独立 GCOV_PREFIX 隔离其输出，未混入覆盖率。
- **覆盖率已完成一轮，补测后待统一重测。** `--coverage` 对 127 个全部 TU 采集 gcov JSON，按 Core 路径/行/分支槽取最大执行计数去重，含头文件，排除 throw 分支；不只收 `.cpp`，不混用旧源码 gcda。前轮 Core 与 Grant/command/authority_intent 组件已达到目标；原始分子/分母、未命中分支、源码前后 SHA 相同证明及逐命令日志保存在 `coverage/`，最终数字须以独立变异收尾后清空 gcda 的统一全套复测为准。`WorkModel::check_grant` 的两条未命中防御分支是在完整链检查成功、同一 owner 无回调/并发变更期间，第二遍链突然缺父或耗尽；其 14/16 计数及解释保留，不制造非法并发刷覆盖，也不从总分母扣除。
- **独立审查已执行。** 全 C 只读审查和两个模块交叉审查分别在 `review.md`、`kernel-cross-review.md`、`scope-cross-review.md`。两项 P2 已修复并静态回看：同批多命令结果破坏 snapshot 严格序号导出；snapshot auth mapping 缺少 OwnerBinding/seq、结果 ID 和 retained Grant 不可变事实配对。回归验证合法 CRC 批次、外主/同序/非法代际/不一致事实拒绝，并保留普通 v1 外主、已回收结果、parent 后来到期的合法对照。新增 Kernel 状态没有散装授权 flag 或 scratch：Grant 数据归 GrantTable，canonical/window/scratch 归 CommandTable；布局、记录和预算公式复用各 owner。删除注释、字段属主、重复公式、裸数字四项复核无待处理阻断问题。
- **独立变异正在收尾。** 另一会话在阅读新增测试前按生产源码/契约冻结 **50 项**清单，原始选点、SHA、每项运行输出、源码恢复和汇总分别留存。初轮存活与失败信号不覆盖；真实缺口补测后在原选点独立重跑，等价项须附源码证明。当前不填写尚未闭合的最终检出率，也不把 50 个选点当作 50 项通过；最终完整复跑及汇总后再更新本节。
- **行为边界。** 已有回归覆盖非零 command、MANAGEMENT/VOICE、root/parent revision、flags、typed caller-owned 输出；完整父链/主体、范围、depth、未持久父、deadline 收窄/溢出、即时失效与启动结算；独立原始命令、省略/排序/去重、摘要碰撞、旧对象失效或回收后的原结果；floor、拒绝原子性、Grant/mapping 同批各断点；snapshot/rotation、原始 stamp、终态次序、精确 replacement、child 引用、易失 hold/deadline 不导出、v1、REQUIRED/covered history、配置缩小准确受限。测试名及条款映射以契约追踪为准。
- **未执行/交付边界。** C 尚未 PR、CI 或合并。撤销 Gate/receipt、Goal 批准和完整工作准入仍随 D/E/F；ESP-IDF 存储、烧录、硬件资源/耐久、真实派发或物理停止、完整 E1–E4 未验收。B #26 的最终八项 CI 含固件编译，只证明 B 对应源码，不能替代 C 的固件编译。
