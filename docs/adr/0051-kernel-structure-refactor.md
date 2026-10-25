# ADR-0051：Kernel 结构整改

- **状态**：已接受（只改结构、不改运行行为；用户 2026-10-25 同意按架构审查第 6 节顺序整改，6 步合为一个 PR）
- **日期**：2026-10-25
- **关联**：ADR-0008、ADR-0011、ADR-0014；`AGENTS.md` 模块设计与可读性要求；架构静态审查（基线 `e089a22`）F02、F03、F08、F13、F20

## 背景

架构静态审查发现，Core 的外层依赖方向正确，问题集中在 `core/kernel` 内部：

- `tools/check_deps.py` 对模块根目录不设约束：子目录可以包含根目录文件，根目录也可以包含任何子目录，所以 `core_subdir_deps` 只描述了真实依赖的一部分。kernel 内已经形成目录级的环，例如 `intent/decision.cpp → call_runtime.h → intent/work_model.h`、`boot → work_replay.h`（根）`→ boot`。ADR-0011“依赖规则由工具保证”在 kernel 内部不成立。
- 一个存储的内存位置要在 5 处同步（`KernelStorage` 的 31 个 `void*`、`KernelLayout` 的偏移、`layout_for`、`ayni_create` 的按位置花括号初始化、构造函数），多字段的尺寸参数由布局、构造和批次预算分别推导。任何一处错位都会让两个存储共用调用方内存块的同一段字节；ASan 把整块视为一次分配，发现不了。
- 外呼标志 `calling_out` 在 10 处手工成对赋值，只因每处都是直线代码才正确，嵌套外呼会清掉外层的标记。

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

## 后果

- 依赖检查覆盖模块根目录；新增根目录与子目录之间的包含须先在 `architecture.json` 声明，引入环的声明会让检查直接失败。在未整改的 main 上运行新规则会报出 62 处未声明的包含。
- 23 个测试文件原有 345 处 `kernel->member` 访问，现全部经 probe；只有 `kernel_layout_test` 为取 `sizeof(ayni_kernel)` 直接包含 `kernel_state.h`。白盒测试按新路径包含 kernel 头文件；只建工作模型的测试夹具用同一组尺寸函数分配存储。夹具原先只按 Grant 命令上限分配命令窗口，小于 `WorkModel` 实际使用的精确命令步长，现已一致。
- 内存布局的放置顺序与各存储尺寸不变：`tools/measure_kernel_memory.c` 的 4 个配置在整改前后得到相同的字节数与对齐。ABI、存储 schema 与持久记录均不变。

## 备选方案

- **把现有违规记为基线，逐步清零**：需要在工具中新增白名单机制，并让声明图继续与真实图不一致；kernel 的环通过迁移文件即可消除，迁移本身不改行为，故不采用。
- **继续默认“根目录可包含任何子目录”，只约束子目录到根目录**：`catalog` 的根目录是被各子目录共用的底层（`catalog_id.h`），`kernel` 与 `work` 的根目录是上层，单一默认无法同时描述二者，故要求两个方向都声明。
- **各存储构造时检查区间大小**：Core 没有断言机制，构造函数也无法返回状态；改为由单一尺寸来源保证一致，并由 arena 审计测试检查区间互不重叠。
- **用 X 宏生成存储清单**：可以只写一处，但引入宏生成的声明，可读性差，故用枚举加数组。

## 验证

- `tests/tools/test_check_deps.py` 新增根目录声明、子目录经根目录绕行（含相对路径）、三种声明环和共享目标无环的场景；`check_deps_repository` 对整个仓库通过。
- `kernel_layout_test` 对默认、最小与全部可选存储启用三种容量检查：各区间按 `Store` 顺序位于 kernel 之后、对齐、互不重叠，尺寸等于构造函数所用参数算出的尺寸，`storage_at` 交出的地址即区间起点。
- `WorkModelCallout` 白盒测试嵌套外呼保留外层标记。
- 迁移到 probe 后测试数量与结果不变；搜索 `tests/` 中 `kernel()->`、`kernel_->`、`kernel->` 后接 `ayni_kernel` 成员的写法，结果为 0。
- 现有完整测试在 GCC ASan/UBSan 下通过；命令与结果见 PR。
