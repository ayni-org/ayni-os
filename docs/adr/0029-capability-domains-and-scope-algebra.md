# ADR-0029：权限与审批（二）：Capability 目录与有限范围代数

- **状态**：已实现并验证；PR [#26](https://github.com/RobotSe7en/ayni/pull/26) 已合并（`00a4126`），最终提交 `8fcc096` 的 CI run 39726008701 八项通过
- **日期**：2026-10-22
- **关联**：计划 §2.2、§2.3、§3.2、§4.2、§9.1、§14.1；`MA-006` B；ADR-0008、ADR-0014、ADR-0028；C-AUTH-001、003～005、008、016；C-LIMIT-001～004；C-CALL-001

## 背景

ADR-0028 将本包分为六个顺序 PR。A 建立可信所有者和会话入口，但尚无 Capability 字段目录或 Grant。C 的受限委派、E 的 Goal 需求以及 F 的工作复核须共用一个有界的范围机制，不能分别解释字符串、JSON 或策略回调来判断权限。

现有目录以 CatalogId 和版本为身份，配置阶段注册、启动后冻结，存储全部来自调用方有界内存。首版只需整数闭区间与有限符号集合，尚无足以支撑表达式语言、单位转换框架或插件化校验器的需求。

## 决策

### 1. B 的接口与边界

- ABI 升为 0.17；`include/ayni/authority.h` 承载 `ayni_capability_desc_v1`、授权字段/值集合及 `ayni_scope_v1`，新增 `ayni_register_capability`。所有新结构按本 minor 的明确布局检查，C99 头文件不暴露 STL、异常、OS 句柄或 C++ 对象。
- 描述符只公开能力 ID/版本及授权 schema。本部分注册能力不创建 Grant，不接受审批，不接入 Provider，也不能由此标记完整 C-CALL-001 已实现。草案中同名大描述符不能与公开定义并存；其余调用属性留在另名的后续草案，由 `MA-008` 继续扩展同一目录。
- 注册只在所有者线程、回调之外、`CONFIGURING` 接受；小型输入与全部数组/字符串返回前复制，启动后不可替换。允许同 ID 多版本，拒绝相同 ID+版本重复；零版本、非法结构/字段/域或保留位明确拒绝，不取工作 ID、不提交 transition、不唤醒 Host。
- 范围规范化、包含与实际请求完整性为 `authority/scope` 的内部机制，供 C/E/F 使用；不为尚无消费者的预览需求增加公开编码/比较 API。比较成立仅表示范围关系，不能替代主体、期限、撤销、预算与实际派发准入。内部同时提供 typed 输入和规范字节输入的覆盖/请求检查，后者先验证规范形状与注册域，直接服务后续持久 Grant 的消费，不另写恢复专用的包含规则。

### 2. 有限字段、单位与实际请求

- Capability 的非零 field ID 在该能力版本内稳定且唯一，最多 8 个授权字段。值类型为 `I64`、`ENUM`、`IDENTIFIER`；后两者虽然都使用有限文本集合，类型不能互换。
- I64 用闭区间表达，端点相等就是等值；ENUM/IDENTIFIER 用非空有限集合表达，单成员就是等值。集合输入至多 8 项，先检查原始 count 再排序去重，不能以大量重复项绕过工作量上限。
- token 与 unit 沿用 CatalogId 的 1–32 B 字符规则。I64 的单位必须非空，如 `count`、`ms`、`mrad` 或无量纲 `scalar`；ENUM/IDENTIFIER 的单位必须为空。单位精确匹配，不做隐式换算；整数定标和业务输入转换属于应用/Provider 边界。
- 字段用途为参数、执行目标、外发目的地。后两者必须为 IDENTIFIER，各至多一个字段，共占 8 字段限额；它们同样只接受注册有限域内的精确集合，不实现前缀或通配匹配。
- 字段之间取合取。Scope 可省略某字段，含义仅为该字段的完整已注册域；未知/重复字段、空集合、类型/单位不符、区间倒置或超出注册域都拒绝，不悄悄截断。
- 所有**已声明的授权相关字段**均是实际请求必填字段；不相关的可选业务参数不纳入此 schema。实际请求每字段只能是单点整数或一个规范 token，必须恰好提供全部字段，不能因 allowed Scope 省略字段便忽略实际请求缺项。
- I64 覆盖比较两个闭区间端点，集合覆盖逐成员检查，多个字段取合取；不使用可能溢出的端点减法，也不把两个 Grant 的片段拼成一条扩大权限的范围。

### 3. 范围规范字节与容量

- 字段按 ID、token 按字节序排序去重，省略字段填注册全域，因此省略与显式全域产生同一规范表达。相同规范字节可供后续确切幂等比较，不能用哈希相等代替内容相等或包含判定。
- 字段域格式独立版本化，当前格式 1；头为 `u16 format`、`u8 field_count`、`u8 reserved=0`，每字段为 `u32 field_id`、`u8 kind/usage/unit_length/member_count`、unit 正文，再跟 I64 的两个 64 位小端端点，或 token 集合的逐项 `u8 length + bytes`。I64 的 member_count 为 0，token 为 1–8。读入要求严格规范、无缺失、无尾字节，不持久化 C++ 布局。
- `capability_capacity` 默认 16、范围 1–256；`authority_scope_max_bytes` 默认 256 B、范围 64–4096 B，追加至配置尾部。字段未覆盖取默认，显式 0 非法。
- 完整注册域本身就是最宽 Scope，必须同样适配该字节上限；否则注册返回容量错误。默认字节预算不保证容纳所有字段/成员同时取最大长度的组合，调用方可明确提高配置，不能先接受无法有界表示的全域。
- Capability 元数据使用现有 BoundedArray，域正文每条一个固定字节 stride；不用每条嵌套 `8×8×32` 的大对象。容量公式只由 registry 的 `storage_size` 定义，layout 使用它。同一 GCC 13/x86-64 实测默认/最小内存较 A 分别 +4,832 B/+140 B；不据 Native 数字声称达到 ESP32 RAM 门槛。

### 4. 状态归属与依赖

- `catalog/capability` 拥有 ID/版本、字段类型/单位/用途、有限域及其规范表示；不判断主体权限、不保存 Grant。领域字节 codec 只表达有限值，不是另一套持久提交器。
- `authority/scope` 拥有规范 Scope、覆盖/收窄与实际请求检查，依赖 catalog 的只读视图；catalog 不反向依赖 authority。类型相同不导致域规则与授权关系重复维护。
- 新 registry 沿 `KernelCapacities → KernelLayout → KernelStorage → WorkModel` 接线，与已有 goal_types/strategies 一致；内核对象不散加 capability flags、重复配置、第二套目录或临时 arena。
- 规范化先校验/计量，再写调用方的有界输出；比较按最多 8 项的局部视图逐字段推进，不在栈上复制两份 4096 B Scope，也不引入 Kernel scratch 状态。

### 5. 兼容与后续部分

B 的目录每次配置阶段重建，没有新的持久工作引用，不新增 journal/snapshot record，不改变当前工作批次语义。C 保存 Capability ID/版本和规范 Scope 后，须在恢复核对版本及完整元数据；缺目录或不能证明兼容时保留并禁行，不能以缺记录解释为无权限需求。

Scene、次数和费用额度不在此有限 Scope 的字段中；不能把“没有额度字段”解释为无限预算。依赖 Scene 的授权仍不支持，额度接入随 `MA-018`。未实现的 Grant/审批/调用入口继续明确拒绝；B 不提供接受后忽略这些功能的路径。

## 后果

- 同一范围机制可由 Grant、Goal 需求与后续真实请求共享，而目录不依赖具体业务、Host 或 Provider；更换 Provider 只需按真实 Binding 提取实际目标和外发字段，不能重定义范围语义。
- 授权 schema 是业务输入中的有限子集，不是完整调用输入/输出 Schema。符号和定标单位需在能力版本中固定；业务结构变更不能悄悄复用旧字段 ID/版本。
- 每能力固定字节 stride 使容量和拒绝结果可预测，但简单域也占配置上限；这是有界内存与实现清晰度的取舍；实测默认/最小调用方内存 67,544/29,060 B。
- 本地验证与独立审查不等于 CI、合并或全包完成；全包机制完成仍须 F，真实屏障后复核、在途取消/外发与物理停止证据仍属于后续包。

## 备选方案

- 任意 JSON Schema、正则或表达式解释器：扩大 parser、包含判定与资源边界，当前有限字段足够，拒绝。
- 每个 GoalType/Provider 自己判断 scope 包含：会产生互相不一致的授权语义，拒绝；业务只声明需求/提取值。
- 全局单位换算/量纲推理框架：当前只需要精确单位，转换留在边界，拒绝。
- 每 Capability 固定嵌套最大字段/最大字符串对象：默认少量简单能力也需大量 RAM，选元数据加字节 stride。
- 为 Scope 新建通用 allocator、scratch manager 或动态注册表：现有调用方内存布局与两遍有界遍历足够，不引入。
- 直接把大草案 Capability 描述符全部变为已实现：资源/Provider/调用契约尚未闭合，会接受后忽略字段；只发布本部分已有机制的明确描述符。

## 验证

原始脚本、日志、JSON、独立审查与变异清单保存在仓库外 `../ayni-ma006-evidence/b/`。以下为最终 B 源码的 Native 证据，A 已合并 PR #25（`f297941`）的证据继续见 ADR-0028。

- **工具链与资源。** `ayni-m0-tools:latest`，GCC 13.3.0、Clang/clang-format 18.1.3、CMake 3.31.6、Ninja 1.11.1、Python 3.12.3。开始时宿主约 18 GiB 可用、swap 635 MiB，监测无持续换入换出。B 验证与独立变异各自顺序占用 2 CPU/3 GiB，memory-swap 同为 3 GiB，`CMAKE_BUILD_PARALLEL_LEVEL=2`、CTest 并行 2；与 C 容器合计 4 CPU/6 GiB，留出宿主余量。TSan 运行时 `vm.mmap_rnd_bits=28`，沿既有开发环境约束，不更改测试。
- **最终五配置。** 显式 GCC 13 配置 `native-debug`、`native-release`、`native-asan`、`native-tsan`，Clang 18 配置 `build/native-clang`；都用 `/gtest` 的固定 GoogleTest 1.17.0 离线源并启用 warnings-as-errors。逐配置 build 后运行完整 CTest，均 **678/678** 通过（665 个单元及 13 个既有 ABI/头文件/依赖/契约等检查）；ASan+UBSan、TSan 没有报告。原命令与工具版本在 `final-validation/run.py`、`results.json`、逐步日志；模拟器退出 0。文档更新后的格式、依赖、契约/追踪与 diff 检查另附同目录。
- **范围行为。** 独立固定小端字节预期、全部截断位置、严格字段/成员次序、保留位/尾字节/版本/usage、I64 的 MIN/0/MAX 等值与闭边界均有测试；数值关系用小有限域逐点 oracle，token 用独立 bitset 子集 oracle。typed 与 encoded 消费者均拒绝非规范、越域、类型/单位/用途改变；实际请求必须含全部授权字段且值为单点。target/egress 均为 IDENTIFIER、同值互换仍拒绝，避免由类型错误掩盖用途核对。规范化先完整验证，再写输出，错误不发布半份结果。
- **目录与 API。** 输入返回前复制；同 ID 多版本、重复身份、注册冻结、回调重入、嵌套错误优先、空域/上限、返回容量错误后旧条目完整、填满 1/2/4 条目录后逐项复验元数据/域/查找都有场景。Core 无新增堆引用，公开 C99/头文件自包含与 Native ABI 布局断言通过；断言按指针宽度表达 32/64 位关系，这不是单独的 32 位编译或硬件运行证据。
- **内存。** 同一 GCC 13.3.0/x86-64 C99 查询程序，默认 `struct_size=12` 为 **67,544 B**，较 A 的 62,712 B **+4,832 B**；最小工作容量（新 Capability 容量 1、Scope 64 B，其余按 workflow）为 **29,060 B**，较 A 的 28,920 B **+140 B**，对齐均 8 B（`memory.c`、`memory.log`）。metadata/域字节 stride 通过同一 registry 容量公式进入 layout，未向 `ayni_kernel` 添加散字段。
- **独立变异。** 另一会话先按源码/契约设计 **35 项**，原清单、仅格式空白适配及每项原始输出均保留。初轮 **30 项检出、5 项未检出**；为下列真实行为缺口补五个回归后，对各项独立重跑，最终 **35 项全部检出，0 编译失败、0 等价或未解释存活**：B13 非递增字段 ID；B14 非递增/重复 token 的每个消费者；B16 encoded 单点整数域；B19 填满 registry 后复验全部先前域/查找；B34 同为 IDENTIFIER 的 target/egress 用途互换。没有因测试结果改写缺陷含义。初轮完整 660/664 项的原未检出证据与补测 664/665 项结果分开；finally 恢复后 **665/665** 通过，六个生产 target SHA 与正常 B 完全一致（`mutation-run/summary.json`、`mutation-rerun/`、`mutation-rerun-b34/`）。
- **覆盖率及偏差。** GCC 13 `--coverage` 完整 **678/678** 通过，收集全部 117 个 coverage 对象的 gcov JSON，按同 Core 路径/行/分支槽最大执行次数合并，排除 throw 分支（`coverage/summary.json`）。Core 含头文件行 **5,415/5,575 = 97.13%**、分支 **3,401/3,793 = 89.67%**；本 B 新增 scope/Capability 三个 `.cpp` 行 **398/410 = 97.07%**、分支 **334/366 = 91.26%**。本 B 含头文件行 **405/417 = 97.12%**、分支 **339/372 = 91.13%**；scope、field_domain、registry 三个 `.cpp` 分支分别 91.18%、90.82%、94.12%，均超过 90%。各文件数字与未命中位置保留；整体恢复/快照低于 90% 的既有偏差仍按 ADR-0028 随 MA-022 补齐，不能用变异检出率代替覆盖率。
- **独立审查。** `review.md` 从公开 ABI、布局、arena、codec、typed/encoded 调用链与已有消费者复核，无未处理阻断发现。初版嵌套参数错误被生命周期遮蔽的问题已修，回归保证 RUNNING 状态的畸形 nested 输入仍先返回 INVALID_ARGUMENT。删除注释、Kernel 字段属主、公式复用、裸数字四项无待解问题；没有新 Kernel scratch、授权旗标或平行 allocator。新增配置字段的半字段 struct_size 边界复用现有 read_capacity 机制，尚无两个字段各自的专门组合测试，作为明确验证限制。
- **CI 与未执行边界。** PR [#26](https://github.com/RobotSe7en/ayni/pull/26) 最终提交 `8fcc096` 的 [CI run 39726008701](https://github.com/RobotSe7en/ayni/actions/runs/39726008701) 八项通过，包括 ESP32-S3 固件编译；B 已合并（`00a4126`）。此前 run 39724951376 的八项通过保留为前一轮证据。未烧录、未测硬件 RAM/Flash/耐久或物理停止。B 不产生 Grant、审批、撤销、Provider/Scene/额度或持久授权记录；目录每次配置重建，后续 C 的持久消费不能跳过版本/完整 schema 核对。本地回归和模拟器不能代替完整 E1–E4、真实输出屏障或执行器证据。
