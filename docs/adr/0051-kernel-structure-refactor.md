# ADR-0051：Kernel 结构整改

- **状态**：已接受（只改结构、不改运行行为；用户 2026-10-25 同意按架构审查第 6 节顺序整改，6 步合为一个 PR）
- **日期**：2026-10-25
- **关联**：ADR-0008、ADR-0011、ADR-0014；`AGENTS.md` 模块设计与可读性要求；架构静态审查（基线 `e089a22`）F02、F03、F04、F05、F06、F07、F08、F13、F14、F17、F20；ADR-0052

## 背景

架构静态审查发现，Core 的外层依赖方向正确，问题集中在 `core/kernel` 内部：

- `tools/check_deps.py` 对模块根目录不设约束：子目录可以包含根目录文件，根目录也可以包含任何子目录，所以 `core_subdir_deps` 只描述了真实依赖的一部分。kernel 内已经形成目录级的环，例如 `intent/decision.cpp → call_runtime.h → intent/work_model.h`、`boot → work_replay.h`（根）`→ boot`。ADR-0011“依赖规则由工具保证”在 kernel 内部不成立。
- 一个存储的内存位置要在 5 处同步（`KernelStorage` 的 31 个 `void*`、`KernelLayout` 的偏移、`layout_for`、`ayni_create` 的按位置花括号初始化、构造函数），多字段的尺寸参数由布局、构造和批次预算分别推导。任何一处错位都会让两个存储共用调用方内存块的同一段字节；ASan 把整块视为一次分配，发现不了。
- 外呼标志 `calling_out` 在 10 处手工成对赋值，只因每处都是直线代码才正确，嵌套外呼会清掉外层的标记。
- 每种意图要求哪种会话、命令重试按什么识别，散在 7 个文件的平行种类列表里（`apply_intent` 的管理会话列表与精确列表、`remember_command`、`find_command`、`check_form`、`CommandTable` 的插入与恢复、记录编解码）。列表之间有刻意的差异，例如撤销属于精确命令却不经 `remember_command`，Grant 创建、撤销与目标审批的管理会话由各自的处理函数检查、晚于命令查找，原因都没有写在代码里。漏改其中一处会削弱新命令的授权或改变它的幂等语义。
- `CallRuntime` 一个类承担决策暂存与授权、资源占用、Provider 传输与排空、回执消费、UNKNOWN 对账、等待投影，以及物理执行：Scene 与观测前提、许可签发、续租、执行端轮询、急停和停止事实提交。安全关键的许可与停止链和普通 Provider 调度在同一个类、同一组私有成员里，无法单独审阅和度量覆盖率；MA-008C 的模型调用改动会落在同一个类上。
- 持久化编排集中在 `kernel/persistence` 的几个文件里。新增一个持久存储要同步修改日志编码的顺序、批次预算、头部分类与 REQUIRED 规则、重放分派、快照写端与读端各自的分区枚举、快照测量和两个快照上限，以 call 为例共 17 处；漏改的后果分别是批次超出预算、恢复失败或快照不完整。记录类型编号分散在 14 个头文件里，没有唯一性检查。启动序列 `BootSequence::record` 还解析 Grant 记录、检查其 ID 的启动纪元，这是 Grant 的恢复规则，却放在存储恢复机制里。

## 决策

1. **模块根目录是声明图中的一个节点。** `core_subdir_deps` 用 `_root` 表示模块目录下直接存放的文件，作为来源和目标都要逐对声明；不再默认“根目录可以包含任何子目录”。每个模块声明的图不得有环，`Architecture` 加载配置时检查，发现环即为配置错误（退出码 2）。既然实际包含必须是声明边的子集，声明图无环即保证实际目录图无环。`architecture.json` 现在列出各模块的真实边。
2. **kernel 根目录文件按职责迁入具名子目录**，消除现有的环，而不是把环记为基线：
   - `config/`：`KernelCapacities`、`KernelLimits`、`KernelStorage` 与工作模型各存储的尺寸（`store_limits.h`）；
   - `persistence/`：`work_journal`、`work_replay`、`work_snapshot` 及各 `*_replay`；
   - `call/`：`CallRuntime` 及其各部分；
   - `decision/`：`DecisionBuffer` 与策略决定，它同时使用工作模型和调用运行时，放在二者之上；
   - `ingress_lock.h` 移入 `ingress/`，`transition_clock.h` 移入其唯一使用者所在的 `intent/`。

   根目录只留 C ABI 入口、`ayni_kernel` 聚合（`kernel_state.h`）与内存布局（`kernel_layout`）。迁移只改路径、包含行、头文件保护宏和 CMake 源列表。ADR-0008 中“kernel 职责集中，不再细分”的判断已被实际的子目录数量取代，以本决定为准。
3. **存储清单只写一次。** `enum class Store` 按放置顺序列出全部 31 个存储，并注明属主。`KernelLayout` 为每个 `Store` 记录区间（偏移、字节数、对齐），`storage_at` 按名称把区间交给 `KernelStorage`，取代按位置的花括号初始化。新增一个存储时，清单、放置和构造各改一处。
4. **每个存储的尺寸参数只推导一次。** 工作模型建的存储（Task、命令窗口、Grant、Goal/Task 授权、调用、Scene）由 `config/store_limits.h` 推导，kernel 自己建的存储（决定缓冲、调用运行时、Provider 信箱）由 `kernel_layout.h` 推导；`layout_for`、构造函数、日志批次预算和快照上限调用同一组函数。只由一个容量决定的存储直接读 `KernelCapacities`，不另包函数。为此 `CommandTable` 改为接受 `Limits`；`TaskTable::Limits` 去掉不占内存的模型调用上限，改为构造参数，布局不再依赖它的默认值。
5. **外呼经 `WorkModel::call_out` 进行。** 它在调用期间置位标志，结束后恢复调用前的值，嵌套外呼不会清掉外层标记；标志改为私有，只读访问 `calling_out()`。标志暂留在 `WorkModel`，移交 kernel 门面在后续“kernel 分区与门面”一步处理。

6. **kernel 级白盒测试经 `KernelTestProbe` 访问内部。** `tests/unit/kernel_test_probe.h` 为 Host 表副本、工作模型、重放、启动、日志、写调度、维护、预算、事件入口与完成槽各提供一个访问函数；测试不再直接写 `kernel->member`。内部表示调整时只改这一个文件；`ayni_kernel` 的成员改为私有时把它声明为友元，与 `EventIngressTestPeer` 的做法相同。单一属主的白盒测试（GrantTable、TaskTable 等）不变，行为仍经 C ABI 驱动。

7. **意图种类的授权与幂等属性只写在一张表里。** `kernel/command/intent_traits.h` 为每个种类写一行：
   - 会话规则：任意会话；所有者 MANAGEMENT 会话，作为操作自身规则的第一项检查，所以已接受命令的重试在它之前就返回首次结果；同一会话但在查找命令之前检查，所以重试也需要它（ayni.h 第 4 步）。
   - 命令身份：普通摘要；精确规范字节；精确并保留工作授权结果；精确并先进入待生效撤销控制（ADR-0031）。
   - 已接受命令的结果对象种类。

   `static_assert` 要求每个种类按序各占一行。`apply_intent` 的两处会话检查、摘要与精确查找的分流、`remember_command`、`check_form` 对精确命令非零 command_id 的要求、`CommandTable` 的插入与恢复、`valid_exact_mapping` 的结果对象种类和记录编解码对普通种类的判断都读这张表，不再各列种类。Grant 创建、撤销与目标审批原先由处理函数各自检查管理会话，现在由 `apply_intent` 按表检查，位置不变：撤销在命令查找之后，创建与审批在存储与命令窗口检查之后。各种类的规范编解码、处理函数和恢复配对仍按种类分支：它们的输入类型各不相同，属于各种类自己的代码；恢复配对在“持久存储契约”一步处理。

8. **记录类型集中登记，持久存储按契约接入编排。**
   - `durability/batch/record_types.h` 列出日志与快照载荷的全部记录类型（1～22）并注明属主，`static_assert` 检查互不重复。各属主的类型常量引用它，记录体的含义仍归属主；新类型先在这里取下一个空号。
   - `kernel/persistence/persistent_store.h` 定义持久存储契约：记录类型；本构建能读的版本（区分日志与快照）；按容量算的行数、最大记录体和每次 transition 最多写几条；按 stamp 写出本次变更的行；快照的逐行遍历、写出与读回。`persistent_stores` 是编译期固定的列表，顺序即日志与快照载荷中的顺序，不做运行时注册。日志编码、批次预算、头部分类、快照写出、读回、测量和两个上限都遍历这个列表，不再逐个点名。契约存储的记录类型任何版本都必须带 REQUIRED，与原先对 call、Scene 的规则相同。
   - 先迁移 call 与 Scene：各自的契约、快照读回与日志重放在 `call_store.cpp`、`scene_store.cpp`（由 `call_replay.cpp`、`scene_replay.cpp` 改名）。日志重放仍是 `WorkReplay` 的成员，由 `apply_record` 分派：它们与命令的配对要读写批次配对状态，这些状态属于 kernel 管理的跨属主顺序。
   - 新增一个持久存储时，编排层只改记录类型登记、自己的存储文件、列表中的一项和重放分派的一项；不再改日志编码、批次预算、分类、快照写读与测量。容量、布局与尺寸仍按决定 3、4 各改一处。
9. **Grant 记录的 ID 纪元归重放校验。** 创建记录的 Grant ID 必须属于它之前那条 BOOT_EPOCH 记录的纪元，撤销记录的 Grant ID 纪元非 0 且不晚于它。这两项从 `BootSequence::record` 移到 `WorkReplay::restore_grant`、`restore_revocation`，与 Scene、调用、Goal 授权已有的纪元规则在一处；启动序列只读日志、维护纪元并交出记录。`WorkReplay::on_record` 不再有以 `UINT64_MAX` 代替纪元的重载，调用方必须给出记录之前的纪元：白盒测试按日志里的 BOOT_EPOCH 记录或节点当前纪元传入。

10. **物理执行控制是 `kernel/execution/ExecutionControl`。** 它拥有原 `CallRuntime` 中的执行部分：准入时保存 Scene 代次与观测前提（`prepare`）、派发与续租前的复核（`allowed`）、持久派发屏障后签发许可（`issue`）、结果的依赖是否仍有效、急停优先的控制通道（`advance_urgent`、`advance`、`ready`）、续租、执行端轮询、停止请求与停止事实提交；执行端端口只在这里调用。`CallRuntime` 保留 Provider 传输、回执、对账与调度，在准入、派发、回执和释放各关口询问控制，不再直接读写许可租约。控制只经 `Transport`（持有与否及投递是否超时、是否只剩停止证明、停止持久后结束调用）读取传输状态，两个目录之间没有环：`call → execution → intent`。`RuntimeMode`、`RuntimeAdvance` 移到控制头文件，`CallRuntime::Mode`、`CallRuntime::Advance` 是它们的别名。函数体按原样迁移，判断顺序与外呼位置不变。审查建议的另一半，即调用结果对 Task 状态的传播（`finish_task`、`expire_wait`）移入 work 层，本次未做：它们只调用 `TaskTable` 已有的操作，移动需要新增 work 内部依赖，留待单独决定。远程执行端与跨线程观测输入的模型另见 ADR-0052。

11. **到期工作的搜索按事务失效。** `ayni_step` 每步问一次是否有到期工作；为了不每步扫描全表，上一次“没有到期工作”的结果被保留，原由 `due_work_changed` 标志失效，标志在 10 个文件的 16 处手工置位。漏掉一处，Goal 的派生或过期就会推迟到下一次无关的变化。现在 `WorkModel::next_due_work` 保留这一结果，同时记下搜索时的 `transitions.next_commit_seq()`、`durable_seq`、是否还有 ID 和下次到期时间；任何一项变了或到了那个时间，才重新搜索。工作表的修改都在事务中，事务本身就让结果失效，修改方无需知道到期搜索。事务之外修改工作表的只有恢复：`WorkReplay` 结束时的重新锚定与持有、重载前的丢弃，以及 `CallRuntime` 恢复调用时因绑定变化而持有 Task，这三处调用 `forget_due_work()`。观测发布与资源释放不改工作表，不再置位。意图仍经 `IntentOutcome::due_work` 唤醒拥有者。保留的字段改为私有，与外呼标志相邻放置，`WorkModel` 的尺寸不变。

12. **生产接口不再有哨兵默认值与可空依赖。** `durable_seq = UINT64_MAX`（意为“全部已持久”）原是 `apply_intent`、`WorkModel::tables`、`refresh_due_work`、`awaits_decision` 与 `work::WorkTables` 的默认值，漏传会在编译无报错的情况下削弱绑定与 Grant 检查。现在 `WorkTables` 不再携带 `durable_seq`，只有读它的 `find_due_work` 以参数接收，Goal 授权表改为引用；其余函数都要求调用方给出。`ayni_submit_intent` 一次取样的内容收为 `IntentContext`（进入时的时钟读数、持久前缀、能否开始事务、存储准入状态、时钟、输入前沿采样），`apply_intent` 的参数由 10 个减为 5 个。`OwnerClock` 与 `InputResumeSampler` 改为构造时必须给出上下文与函数的类，删除为空时“改用调用方读数”和“无法采样”的分支；`decide_next` 以引用接收 `CallRuntime`，删除为空时把调用提议判为不支持的分支。`WorkReplay` 的事务与纪元上限不再默认无界，日志配对与进入 RUNNING 时的复查显式传入 `WorkReplay::unbounded`。时钟规则写在 `OwnerClock` 与 `CallRuntime` 的注释里：进入时的读数用于调度和外呼前的检查，外呼返回后的检查重新读时钟。生产调用方原本就传入完整参数，行为不变。work 层单表接口（`GoalTable` 的本地输入触发、`GoalAuthorityTable::bind`）保留可选参数，供不建授权表的单表测试使用；kernel 只经 `WorkModel` 调用它们，并总是给出。

## 后果

- 依赖检查覆盖模块根目录；新增根目录与子目录之间的包含须先在 `architecture.json` 声明，引入环的声明会让检查直接失败。在未整改的 main 上运行新规则会报出 62 处未声明的包含。
- 23 个测试文件原有 345 处 `kernel->member` 访问，现全部经 probe；只有 `kernel_layout_test` 为取 `sizeof(ayni_kernel)` 直接包含 `kernel_state.h`。白盒测试按新路径包含 kernel 头文件；只建工作模型的测试夹具用同一组尺寸函数分配存储。夹具原先只按 Grant 命令上限分配命令窗口，小于 `WorkModel` 实际使用的精确命令步长，现已一致。
- 新增一种意图先在 traits 表加一行；授权与幂等属性在一处审阅。策略矩阵测试逐行写出期望值，改表必须同时改测试。
- 新增持久实体（订阅、记忆、提醒）时，持久化编排要改的位置由 17 处减为上述 4 处（以 call 计），其余存储按同一契约逐个迁移。日志与快照的记录字节、顺序、版本与 REQUIRED 规则不变；快照中 call 先于 Scene 由列表顺序保证，读端仍拒绝逆序。
- 内存布局的放置顺序与各存储尺寸不变。决定 1～9 之后，`tools/measure_kernel_memory.c` 的 4 个配置与整改前得到相同的字节数与对齐；为此 `WorkModel` 把两个到期搜索标志相邻放置，抵消外呼标志改为私有后的对齐填充。决定 10 让 `ayni_kernel` 固定增加 56 字节（`ExecutionControl` 保存自己的协作者引用与传输接口），与容量无关，4 个配置各增加 56 字节。ABI、存储 schema 与持久记录均不变。
- 许可与停止链在一个约 400 行的文件里，可以单独审阅和度量覆盖率；修改 Provider 调度不再改动它。
- 到期判断与下次到期时间总与重新搜索相同。代价是每个事务之后的下一步多做一次有界搜索（Goal 表、每个 Goal 至多 `goal_depth_max` 个祖先、Task 表），以前只在置位的事务之后搜索。原先不置位的持有（ABANDONED 调用持有 Task、恢复中因绑定变化持有 Task）之后，报告的下次时间可能早于实际，只多一次唤醒；现在与重新搜索一致。

## 备选方案

- **把现有违规记为基线，逐步清零**：需要在工具中新增白名单机制，并让声明图继续与真实图不一致；kernel 的环通过迁移文件即可消除，迁移本身不改行为，故不采用。
- **继续默认“根目录可包含任何子目录”，只约束子目录到根目录**：`catalog` 的根目录是被各子目录共用的底层（`catalog_id.h`），`kernel` 与 `work` 的根目录是上层，单一默认无法同时描述二者，故要求两个方向都声明。
- **各存储构造时检查区间大小**：Core 没有断言机制，构造函数也无法返回状态；改为由单一尺寸来源保证一致，并由 arena 审计测试检查区间互不重叠。
- **用 X 宏生成存储清单**：可以只写一处，但引入宏生成的声明，可读性差，故用枚举加数组。
- **`CommandTable` 对（种类, 规范字节）通用，编解码由 traits 提供**：各种类的规范请求类型不同，通用接口需要类型擦除的函数指针；现有 `find_exact` 重载在编译期绑定种类与编解码，授权与幂等属性也不依赖它们，故不采用。
- **契约同时包含日志重放**：call 与 Scene 的重放要读写 `WorkReplay` 的批次配对状态（Scene 必须由同批次的所有者命令收尾，调用的对账请求要与查询命令配对），放进契约就要公开这些状态。待配对改为数据表（每种 transition 需要哪些伙伴记录）后再一并移入，本次不做。
- **运行时注册表**：需要可变的全局状态与初始化顺序，在零堆的嵌入式目标上没有收益；列表在编译期固定。
- **一次迁移全部存储**：Grant、Goal、Task 的日志记录随命令种类变换版本，快照与日志的记录体也不同，宜先有配对数据表；本次只迁移结构最独立的 call 与 Scene，验证契约的形状。
- **Grant 纪元校验移进 `GrantTable`**：`GrantTable` 不知道启动纪元，需要新增参数；Scene、调用与 Goal 授权的纪元规则都在重放里，放在一起便于审阅。
- **`ExecutionControl` 直接持有 `CallRuntime` 的引用**：控制需要读取传输槽，双向引用会让 `call` 与 `execution` 两个目录成环；改用函数指针加上下文的窄接口，与 `TaskTable`、`CallTable` 的保留回调相同。
- **`ayni_step` 的各推进函数共用一个 `StepContext`**：决定需要时钟与调用运行时，到期工作需要授权时刻，存储推进两者都不要，合成一个结构只会让每个函数多出不用的字段；只对字段齐全、参数最多的意图入口这样做。
- **所有管理会话都在命令查找之前检查**：会改变 Grant 创建、撤销与目标审批的重试结果与错误优先级（现在任一会话都能取回已接受命令的首次结果），超出只改结构的范围。表中显式区分两种规则，是否统一另行决定。

## 验证

- `tests/tools/test_check_deps.py` 新增根目录声明、子目录经根目录绕行（含相对路径）、三种声明环和共享目标无环的场景；`check_deps_repository` 对整个仓库通过。
- `kernel_layout_test` 对默认、最小与全部可选存储启用三种容量检查：各区间按 `Store` 顺序位于 kernel 之后、对齐、互不重叠，尺寸等于构造函数所用参数算出的尺寸，`storage_at` 交出的地址即区间起点。
- `WorkModelCallout` 白盒测试嵌套外呼保留外层标记。
- `intent_policy_matrix_test` 逐行写出 16 个种类的期望属性并与表比对；经 C ABI 检查 VOICE 会话恰好被拒绝管理种类、MANAGEMENT 会话通过所有会话规则、精确种类缺 command_id 被拒绝；以 Grant 创建与 Scene 开关各代表一种管理规则检查重试；`CommandTable` 只为普通种类接受和恢复无规范字节的映射。Grant 的信任边界白盒测试改为经 `apply_intent` 注入过期会话事实。
- `persistent_store_test`：列表中每个存储的记录类型都在登记表里且按日志顺序排列；契约存储的头部无论版本是否已知都必须带 REQUIRED，已知版本在日志和快照中都可分类；启用一个存储后，批次上限与快照载荷上限恰好增加它声明的份额，单记录上限不小于它的最大记录。去掉分类中对契约存储的 REQUIRED 检查会让该测试失败。现有日志、快照与恢复测试（含 call、Scene 的写读和逆序拒绝，以及 Grant 未来纪元 ID 的拒绝）不改期望值并通过。
- 抽出 `ExecutionControl` 不改任何测试：`managed_execution_test` 与其余调用、执行测试不变并通过；覆盖率见 PR。
- `WorkTrigger.kept_due_work_search_answers_as_a_new_search_would`：固定种子的 400 步随机操作（创建延时与周期 Goal、暂停、恢复、取消、实例暂停与恢复、推进时钟、step、重启）中，每步之后比较保留的搜索与强制重新搜索的结果与下次时间，并要求其中多于 20 次由保留的搜索给出时间。让保留的搜索忽略事务会使它失败。
- 删除默认值与空分支后，编译器报出的调用点只在测试中（Grant 信任边界测试、重放一致性测试）；生产调用点原本就传入完整参数。
- 迁移到 probe 后测试数量与结果不变；搜索 `tests/` 中 `kernel()->`、`kernel_->`、`kernel->` 后接 `ayni_kernel` 成员的写法，结果为 0。
- 现有完整测试在 GCC ASan/UBSan 下通过；命令与结果见 PR。
