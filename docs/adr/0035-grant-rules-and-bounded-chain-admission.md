# ADR-0035：Grant、命令与 Kernel 编排的状态归属

- **状态**：实施中；R2b1 已验证，R2b2/R2b3 按顺序追加决策与验证。
- **日期**：2026-10-23
- **关联**：`MA-006` R2b1～R2b3；[详细计划](../ma006-implementation.md) §3.2；[复查清单](../../reports/core/ma006-a-d-review.md) A3、C2、C4～C7、D3；ADR-0030/0031/0034；C-AUTH-019～024、026/027。

## 背景

Grant 的结构体、记录与命令分别重复标志和期限规则。创建时父链在 GrantTable 与 WorkModel 中反复遍历；主体可用性也有四份判断。撤销根、后继 ID、终态更新和恢复祖先检查存在多份实现。缺少注释和混合的恢复函数，使错误顺序、日志与快照的不同证据难以核对。

本 PR 整理这些既有机制，不改变公开行为、ABI 或持久格式。命令暂存/回执/重放配对留给 R2b2，Kernel step/快照编排留给 R2b3；三份共用本文，各份分别通过完整流程；全部合并后量一次 R2b 覆盖率（2026-10-23 workflow 第 2 节）。

## 决策

1. **规则留在不变量属主。** `authority/grant/grant_rules.h` 拥有请求已知标志、未实现特性标志、持久允许标志及期限/时长关系。入口仍保留自己的检查次序：未知位是非法输入，已知 Scene/Quota 是明确不支持，存储只能有已支持标志。`AgentTable::usable()` 拥有主体可用性，保持 PAUSED 可作为授权主体、STOPPED/FAULTED 不可用。Kernel 的同一管理端检查供创建和撤销复用，不将会话身份规则下移到 GrantTable。
2. **父链只定位一次，工作主体仍由 Kernel 检查。** GrantTable 的成功 `check_chain()` 发布局部 `CheckedChain`，最多保存最大委派边数加根的索引；失败不改输出。`Admission` 带父链，根创建使用明确的空链。WorkModel 在整个授权链通过后，按这些索引检查主体，不再次遍历或重复授权检查，保留授权/容量错误优先于祖先主体错误的顺序。该值只在单次所有者线程调用内消费；期间禁止表变动、回调或重入，不是跨调用缓存。
3. **共用有界遍历，保留不同的边界含义。** GrantTable 私有 `visit_ancestors()` 只接受内部判定，返回到根、判定停止或断链/超限。完整链和创建历史检查允许最大深度加一个根节点；后代边关系与撤销 frontier 检查允许最大深度。创建历史虽从父开始，仍保留 max+1 预算，以保持降低配置深度时先判有效历史、再返回容量不足的既有错误分类。`descendant_of()` 仍先比较父 ID 再查父槽，完整链仍先验证边再检查父状态。`history_at_creation()` 返回 active/later_terminal/invalid，代替用布尔参数选择日志或快照算法。
4. **父边共同契约与可用证据分开。** `check_parent_contract()` 集中所有者、绑定 revision、exact capability/version、完整源域、再委派许可和期限种类。实时创建比较 deadline；保留事实校验创建 boot、duration 及存在的 deadline；恢复仅有持久 boot/duration。三者共用共同规则，但不假造不存在的时间证据。`no_grant_deadline_us` 明确永久授权或恢复限时授权没有活 deadline，后者不可继续运行。
5. **撤销与回收只有一份选择/更新规则。** `is_revocation_root()` 只比较已有效事实的 root identity，调用方仍检查需要的生命周期。`next_id_after()` 在冻结的 inclusive frontier 内按 ID 选后继，失败不改输出；实时清理与恢复使用同一规则。`revoke_by()` 统一首次终态更新，根的 cleanup stamp 独立推进，不改变终态回收顺序。字节区定位使用唯一 stride 和具名 `grant_value_regions`。
6. **恢复分为检查、放置与发布。** `RestoredState` 将输入的生命周期/revision/两个 stamp 组合；共享事实校验和最终复制。日志创建要求祖先仍活跃并消费记录指定的精确替代对象；快照允许创建后才终止的祖先，要求 predecessor 已不存在，并按父先子后追加。两路分别调用具名步骤，不再传布尔模式。所有可失败检查在发布前完成；损坏与容量不足的分类顺序保持。
7. **会话与兼容。** 公共入口把已查到的 Principal 传给 `open_session()`；Session.binding_revision 明确是打开时捕获的身份快照，后续不能随主体变更自动升级。没有新增 Kernel 字段、持久缓存、堆分配、I/O 或外部回调。ABI 保持 0.19，记录及 arena 布局不变；CheckedChain 是最多 17 个索引加数量的局部值，其实际大小与两组 arena 需求在验证中实测。既有测试断言保持，逐测试补契约引用；机器检查仅移除已消除的四项历史基线。

8. **命令候选与批次配对（R2b2）。** CommandTable 以私有 `optional<StagedCreate>` 组合 scratch 的完整身份及长度；空值明确表示无候选，普通插入、恢复、清空及下一次 exact lookup 统一清除。撤销在已接受 PendingRevoke 中拥有自己的 canonical；先用测试证明查找产生的 scratch 不可发布且无消费者，再删除该副本。重放的 Goal/Task 待配对使用 optional；Grant create、root revoke 与 child cleanup 由互斥 kind 选择，同批检查与 discard 归于 WorkReplay 的 `BatchPairing`。这些只是在原属主内整理短期状态，不改变记录或新增 Kernel 字段。
9. **回执与版本说明（R2b2）。** 查询复用 pending outcome；唯一持久升级依据当前 writer 的已确认 sequence。reload 先清 queued stamp、再恢复窗口、最后发布完成状态，因此不以跨 boot 的公开 floor 推断持久。带命令 submit 从保留回执填充，commandless 从本次 transition 填充。恢复分类注释及静态版本断言覆盖 D 已有记录；补充逐测试契约引用。
10. **内部 optional 的取舍（R2b2）。** C++17 `std::optional` 只存放有界、平凡值，不分配堆；仅使用受前置条件保护的解引用及 reset，不用可抛异常的 `value()`。它不进入 C ABI、wire 或持久记录。架构标准头白名单新增 `optional`，无堆符号和 no-exceptions 构建检查继续执行。相较为每种值另造带布尔标志的容器，标准值类型清楚表达存在性，维护成本更低。

## 后果

- 更改主体状态准入只需改 AgentTable 规则；更改 Grant 标志只改 authority 规则及其入口错误顺序测试；更换 Host 不改变这些 portable Core 规则。
- private visitor 的新增抽象服务五种已有祖先遍历，维护一份循环/断链逻辑；不暴露回调 API，也不创建通用树框架。
- Grant.revision 仍是生命周期对应的持久公开事实，保留字段并明确 ACTIVE/终态的不变量。把它删除会改变表/记录消费者，本次不做。
- `CheckedChain` 增加少量有界临时栈，换取不重复父 ID 查找和授权检查；最大深度的内存大小实测不等于最坏 step 栈水位或时长测试。
- 本次不实现 Goal 审批、活工作引用或授权恢复；后续仍按 R2b2→R2b3→R3→R4→E→F 交付。

## 备选方案

- 保留每个调用者自己的规则与树遍历：无法保证错误优先顺序及边界同步，拒绝。
- GrantTable 直接访问 AgentTable：会使 authority 依赖 work 并混淆不变量属主；改为返回短期索引路径。
- 持久缓存完整祖先链：引入替代/恢复失效维护与 arena 占用；本次只用同一次调用中的局部结果。
- 合并日志与快照放置：两者对终态祖先和 predecessor 的契约不同，保留两个具名入口。

## 验证

### R2b1

GCC Debug、Release、ASan/UBSan、TSan 和 Clang Debug 全部通过；模拟器、工具/依赖/契约/可读性/格式检查及 ESP-IDF v6.0.3 固件编译通过。默认及最小工作容量的 arena 需求未变。

独立会话按生产 diff 与契约提出 25 项植入缺陷，首轮检出 19 项；补六项行为测试后，其余六项全部检出。独立审查的两项注释/规则复用问题均已修复；没有未处理的生产问题。命令、逐项结果和四项可读性审查见 [#37](https://github.com/RobotSe7en/ayni/pull/37)。

本轮依用户授权只运行本地 CI；未进行硬件、ESP-IDF 存储 Host 或 E/F 验收。整个 R2b 的覆盖率在 R2b3 合并后记录。

### R2b2

GCC Debug、Release、ASan/UBSan、TSan 和 Clang Debug 全部通过；模拟器、工具/依赖/契约/可读性/格式检查及 ESP-IDF v6.0.3 固件编译通过。默认/最小工作容量的 arena 需求各增加 8 B，为 110,800/31,624 B，对齐仍为 8 B；这是私有 optional 配对状态的布局变化，ABI 与记录版本未变。

独立会话提出 20 项植入缺陷，首轮检出 13 项；补七项行为测试后，其余七项全部检出。撤销 scratch 删除前的行为证明通过；独立审查的测试契约注释意见已全部处理，无未关闭的生产问题。命令、逐项结果及四项可读性审查见本次 PR；R2b 覆盖率仍在 R2b3 合并后统一测量。
