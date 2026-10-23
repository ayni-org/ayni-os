# ADR-0037：Goal 需求、待批与原子批准

- **状态**：实施中；E 按独立可合并 PR 交付，已交付内部事实、持久闭环与引用保护，继续接入公开需求与批准。
- **日期**：2026-10-23
- **关联**：MA-006 E；[实施计划](../ma006-implementation.md) §6；ADR-0028～0031；计划 §9.1、§14.1。

## 背景

Grant 已有有界目录、Scope、持久身份与撤销机制，Goal 尚未保存需求或授权引用。恢复因此无法区别明确无权限需求和旧记录缺少授权证据，当前仍停下全部恢复的活工作。E 要先建立可恢复的明确上限，再将待批创建、查询、原子批准与最低消费复核一起闭环。

## 决策

1. **拆分与公开边界。** E1 先交付 work/goal 的内部有界事实表和 type 17/v1 codec；此时 Kernel 不分配该表、不写新记录、不开放批准。后续 PR 接需求导出与持久闭环，再接批准事务和消费门控；每份 Core 增删控制在约 1,500 行内，超过继续按属主拆，不提前开放缺少门控的写入口。本部分共用本 ADR。
2. **事实归 work 所有。** 每 Goal 按原需求顺序保存 exact Capability ID/版本、原始完整域、规范 Scope 和一个 Grant ID/revision；每项需求独立由一条 Grant 覆盖，不能拼接。需求数默认 4、最大 16，域和 Scope 各受已有 Scope 字节上限约束。表按 Goal 槽位分配，读取还须匹配 Goal ID，避免槽位替换转移授权。更换 Host 只改变外层 I/O，不改该模型。
3. **一个语义校验入口。** struct 与字节输入共用 valid_goal_authority_record；table 的 install_validated 只检查配置容量并复制已验事实，前置条件明确要求原输入在校验后未变且不与 live 槽位重叠；E2b 的专用暂存字节行与 live 槽位分离，可以作为复制源。codec 解码到局部视图，完整校验后一次发布；writer 消费已验证事实，不重复验证。工作表状态与记录布局分开，但不复制域/范围算法，复用 authority/scope。
4. **明确缺失与状态。** 表缺席是无元数据；存在且 count=0 是明确零需求。proposed 必须至少缺一项 Grant；authorized 和 paused 保留完整引用（可以有零需求）。paused 表示显式授权暂停，引用失效不改写原上限，不自动选择新 Grant。各状态保持元数据，用户暂停及兼容 hold 仍归各自现有属主。
5. **持久格式。** 新记录保留 Goal ID、最后变更 stamp、阶段、需求数与各需求的完整证据。journal 和 snapshot 共用一套 reader/writer，配对、所引用 Goal/Grant 是否存在与当前目录兼容性由集成属主验证，codec 不按新目录重释旧域。E1 只实现 codec；后续进入 REQUIRED 日志及快照时同步接入容量、发现和重放规则。
6. **部分已有授权（产品语义，用户选定推荐项，2026-10-22）。** 继续依赖已有 Grant，只为缺项创建默认不可委派的 Goal 专属 Grant。撤销原 Grant 仍暂停该 Goal；审批依据不得悄悄增加缺项。具体事务与命令规范身份在对应 PR 追加。

7. **E2a 持久基础。** Kernel 配置追加 `goal_requirement_capacity`，ABI 升至 0.20；完整配置必须填 1～16，短前缀不含该字段时取 4。运行时与所有仓库内调用者同步更新；旧 0.19 二进制调用者按 C-VER-006 重新编译。KernelStorage/Layout 分配表，实例由已有 WorkModel 持有，Kernel 对象不另增字段。
8. **可辨别的记录配对。** GOAL type 3/v2 的正文保持 v1 布局，但 REQUIRED envelope 声明必须有同批 type 17/v1；journal 先 Goal/状态再元数据，snapshot 中 Goal 与元数据相邻（允许跨页）。旧 v1 缺席继续为未知，不能静默变成零需求。每 transition 最多一条 type 17，批次预留按一条最大记录计算；后续逐 Goal 的有界清理复用这个界限。配对、正文 stamp、原需求不变、阶段转换、Grant 的原主体/版本/域/范围与存在性在恢复时校验；终态工作可保留已合法回收的 Grant 历史引用，活工作不可以。
9. **回收保护归属。** WorkModel 从当前工作事实生成局部 128 B Grant 槽位排除集，普通创建与日志精确替代共用它；不持久化计数，不让 authority 读取 work。活工作始终保护引用；终态工作在最后 stamp 被当前 writer 确认后停止保护。覆盖引用或复用 Goal 槽位先完成全部可失败检查，再把旧引用的释放 seq 交给 GrantTable；每 Grant 只保留最大的易失释放 fence。live 回收要求 fence 不高于实际 `CommitLog::durable_seq`，日志要求新建 seq 严格大于 fence，禁止同批释放后复用。
10. **失败、重载与快照。** fence 随 discarded Grant 行清除，由已接受日志重建；不依赖失败分支曾分配的 seq 永不复用。快照在 writer 空闲的已持久边界发布，不保存 fence；尾日志继续重建它。恢复失败仍丢弃全部部分事实。E2a 不开放需求/批准入口，PROPOSED 的 Goal 状态持久路径随 E2b 一起闭合；仍保持所有恢复活工作的现有 hold；有效新工作正常恢复由 F 闭环。旧 reader 不能读取 REQUIRED type 3/v2 或 17/v1，回退旧固件只可安全受限，不原地删除新记录来降级。

11. **E2b 导出与暂存。** ABI 0.21 在 GoalType 末尾增加 `export_requirements`；空回调明确表示新 Goal 零需求，不解释旧记录缺席。参数先校验，回调在 owner 线程且禁止重入；sink 每次 emit 立即规范化并复制输入，首个错误不可被后续 emit 或回调成功掩盖。错误保留原分类；所有检查成功才分配 Goal ID/stamp。GoalAuthorityTable arena 额外预留一行配置大小的字节暂存，不增加 Kernel 字段，不在栈上放最大 Scope 数组。目录保存回调，Kernel 协调选 Grant，work 保存不可变需求；scope 算法仍归 authority。
12. **待批与当前消费。** 每项选择同主体、完整覆盖且当前持久有效的最小 ID Grant；已选引用保留，缺项进入 PROPOSED。PROPOSED 无触发游标、不收本地输入、不派生 Task、不能有子 Goal，可取消并按原期限失效。子 Goal 每项须被一条原父需求覆盖，不能拼接；零需求父不提供额外权限。派生前、策略调用前及返回后检查原引用和祖先；导出和策略回调后重新读 Host 单调时钟，过期输出连同步骤/检查点一起丢弃。触发节拍仍按 step 起始时间，不把授权检查时钟混入调度。
13. **两种暂停及版本边界。** Goal.lifecycle 继续保存普通用户/工作生命周期，GoalAuthorityPhase 独立保存授权暂停；查询组合为有效 PAUSED，不复制有效状态，也不覆盖兼容性 hold。自动授权暂停像触发推进一样更新 commit_seq，保留普通命令 revision；Task 按现有传播规则暂停，饱和 revision 的 Task 已不能执行，保持原值。普通 GOAL_RESUME 不能清除授权阶段。新创建/快照写 GOAL type 3/v3；有元数据 Goal 的后续状态写 GOAL_STATE type 4/v2，均 REQUIRED，正文布局不变；type 17 继续 v1。仅升 GOAL 版本不能保护旧 v2 Goal 的后续更新，因此两种 envelope 都升级。旧 reader 安全拒绝，新 reader 保留 v1/v2 的原生命周期，不把旧 PAUSED 猜成 ACTIVE。
14. **查询与恢复范围。** 三个只读接口复制摘要、按原顺序的需求及每字段完整原域/Scope；错误不写半份输出。保存的 phase 与当前缺项/失效原因分开。PROPOSED 的 journal/snapshot 恢复、配对及禁止 Task/子 Goal 一起接入；已验证待批工作不需施加 LEGACY_UNCONFIRMED，但原 GoalType 兼容 hold 保留。活跃工作正常恢复、Task 独立引用和完整撤销清理仍由 F 交付；E2b 不开放批准或重新授权入口。

15. **批准默认期限。** 用户 2026-10-23 确认缺项专属 Grant 默认长期有效，重启后仍可用；管理端可明确选择更短期限或撤销。Goal 终态立即使其不可消费，物理回收仍受持久和引用保护约束。此批准行为随 E3 交付。

## 后果

- work 新增对 authority 的向下构建依赖，仅复用 Scope 关系；authority 不依赖 work。E1 不增加 Kernel 字段或 arena 占用；E2a 的内存增长来自明确容量的表、Grant fence 与原日志预算。
- 局部排除集只增加固定栈空间，不维护第二份引用计数。当前按有界 Goal 需求逐项查 Grant；默认规模与绝对上限的 Native 扫描成本已量测，见 E2a PR。最大配置及 ESP32 的实时预算仍须按部署测量，不能由默认配置或有界性推断满足。
- E1 的直接模块测试证明表示、容量和编解码规则，不是管理端审批、断电事务或正常恢复工作已实现的证据。
- 原 ABI、日志、快照读取与写入行为保持；新模块没有被 Kernel 启用，因此 E1 不改变旧调用者或部署镜像的持久格式。E2a 的 ABI/记录兼容边界见决策 7、8、10；E2b 公开创建产生显式元数据，兼容边界见决策 11～14。

## 备选方案

- 每个 Grant 保存持久引用计数：重复工作事实且让恢复多维护一套一致性，拒绝；局部排除集与易失 fence 足够。
- fence 额外保存 boot：重载已清整行且 live 使用当前 writer 确认，单 seq 足够；失败分支 seq 复用由 discard/replay 隔离。
- 每批为所有 Goal 的完整需求预留空间：E 每步只更新一个 Goal，会无谓放大默认 arena，采用单记录上限并由 writer/reader 执行。

- 缺记录等于零需求：将旧未知工作直接放行，拒绝。
- Goal 只保存当前 Capability ID 并在恢复时重新导出：会按漂移目录解释旧授权，拒绝。
- Kernel 新增平行权限字段或 authority 读取 GoalTable：混淆工作事实属主或倒置依赖，拒绝。
- 导出回调返回借用 Scope 数组：可能引用已退出的栈帧；改为同步 sink，立即复制每项。
- 单独给 GOAL v3 升级：无法让旧 reader 拒绝只含 GOAL_STATE 的新语义更新；同时使用 GOAL_STATE v2。
- 每个需求分配最大 Scope 数组：会让小配置付出最大容量代价；采用配置决定的 arena stride。

## 验证

E1（[#42](https://github.com/RobotSe7en/ayni/pull/42)）：GCC Debug/Release、ASan+UBSan、TSan 与 Clang Debug，以及模拟器、工具检查和 ESP-IDF 6.0.3 固件编译均通过。独立变异 20 项，首轮检出 15 项，5 项补测后全部检出；独立审查无未关闭发现。默认/最小工作配置内存需求未变，仍为 110,800/31,624 B，对齐 8 B。未执行远端 CI、硬件或持久集成验证；E 全部 PR 合并后统一测量覆盖率。

E2a（[#43](https://github.com/RobotSe7en/ayni/pull/43)）：GCC Debug/Release、ASan+UBSan、TSan 与 Clang Debug，以及模拟器、工具检查和 ESP-IDF 6.0.3 固件编译均通过。独立变异 25 项，首轮检出 15 项，10 项补测后全部检出；独立审查无未关闭发现。默认/最小工作配置内存需求为 193,136/32,640 B，分别增加 82,336/1,016 B，对齐仍为 8 B。新增场景验证日志/分页快照配对、原引用与释放 fence，以及 FAILED/UNKNOWN 后实际重载的丢失和已写入分支。未执行远端 CI、硬件或 ESP32 时限测量；公开需求创建、PROPOSED 状态恢复及批准仍由后续 E PR 交付，覆盖率在 E 完成后测量。
