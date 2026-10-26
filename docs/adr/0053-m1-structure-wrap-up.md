# ADR-0053：M1 结构收尾——主版本 0 只收当前 ABI 布局、精确命令查找、调用传播与共享默认配置

- **状态**：已接受（用户 2026-10-26 在 M1 详细实施计划中选定“0.x 只收当前布局”，并要求开始 P0）
- **日期**：2026-10-26
- **关联**：ADR-0008、ADR-0010、ADR-0026、ADR-0030、ADR-0031、ADR-0042、ADR-0051；架构静态审查（基线 `e089a22`）F05、F06、F11、F16、F19；`contracts/domain/versions.md` C-VER-001/003/004/006；`contracts/host/host_ports.md` C-HOST-015/016

## 背景

M1 剩余工作从 P1 起新增模型 Provider、工具调用、记忆、日历与订阅。架构审查中有 5 项与这些新功能直接相关，留到新功能里处理会让新代码沿用旧结构：

- **F11 两套 ABI 兼容机制。** C-VER-001/006 规定主版本 0 下不同 minor 互不接受，调用方必须按当前头文件重编译；`kernel_api.cpp` 却仍为配置、Host 表、Kernel 信息、step 报告和完成结果保留 0.1～0.14 的最小尺寸、字段终点表和“未覆盖字段取默认值”的读取。0.5 以后新增的结构体都只接受自身的 `sizeof`。两套规则并存，每个新字段都要判断属于哪一套；按当前头文件编译的调用方又从不使用旧布局，这部分代码只被专门构造的测试走到。
- **F05 精确命令查找。** `CommandTable` 为 6 种精确命令和撤销各有一个 `find_exact` 重载，种类与规范编解码在重载里绑定。P1 之后每个新的所有者命令（记忆删除、暂停助理等）都要给命令表加一个重载。ADR-0051 否决过“命令表对（种类, 规范字节）通用、编解码由 traits 提供”，理由是要用类型擦除的函数指针。
- **F06 调用运行时中的 Task 传播。** ADR-0051 决定 10 抽出了 `ExecutionControl`，留下另一半：调用的终态、对账答复与资源等待跳过如何改变 Task（`finish_task`、`expire_wait` 与对账里的判断）仍是 `CallRuntime` 的私有函数。P1 的工具调用会在调用运行时上继续加代码。
- **F16 组合根的默认配置。** `native_node` 与 `esp_idf_node` 各自手写同样的 24 行配置；P2 的单进程 CLI 与 SimNode 还会再写一份。
- **F19 文档与草案漂移。** `core/README.md` 的模块表仍说 knowledge 没有代码、resources 未接消费链、work 的调用随后续包，内存数字停在 ABI 0.19；`AGENTS.md` 仍列出已取消的 `budget/`；草案 `control_v1.h` 的 `AYNI_TARGET_VERIFIED/ESTIMATED/UNKNOWN`（1/2/3）与正式 `execution.h` 的 `AYNI_TARGET_UNVERIFIED/SATISFIED/UNSATISFIED`（0/1/2）同名不同值。

## 决策

1. **主版本 0 的每个 ABI 结构体只有本次 minor 的布局。** 配置、Host 表、Kernel 信息、step 报告和完成结果与其余结构体相同：`struct_size` 小于 `sizeof` 为 `AYNI_INVALID_ARGUMENT`，大于为 `AYNI_UNSUPPORTED_VERSION`。删除旧布局的最小尺寸、字段终点表、部分写出和“未覆盖字段取默认值”；配置的每个容量字段都必须给出且在范围内，允许 0 的字段仍由 0 取默认。`abi_major` 与 `abi_minor` 在每个布局中位置不变（C-VER-002），所以 `ayni_create` 与 `ayni_kernel_memory_requirements` 先在 `struct_size` 足以包含 `abi_minor` 时比较版本，再判断尺寸：其他 minor 的调用方无论配置长短都得到 `AYNI_UNSUPPORTED_VERSION`；连 `abi_minor` 都不含的配置（0.1）得到 `AYNI_INVALID_ARGUMENT`，原先为 `AYNI_UNSUPPORTED_VERSION`。布局与 `AYNI_ABI_MINOR` 不变：按 0.36 头文件编译的调用方填写 `sizeof`，行为不变；其他 minor 的调用方原本就被拒绝。0.36 头文件原注释允许 Kernel 信息与 step 报告以较短尺寸部分写出、配置省略尾部字段取默认值，按这些注释调用的 0.36 调用方现在得到 `AYNI_INVALID_ARGUMENT`。这是 C-VER-006“每次变更递增 minor”的一次例外：仓库内的调用方都填写 `sizeof`，ABI 尚未对外发布，不为只删除旧布局而递增 minor；下一次布局变化（P1 的 0.37）照常递增。主版本 ≥ 1 时 minor 只追加，届时再引入统一的前缀兼容读取，供所有结构体共用（C-VER-006）。`catalog::check_struct_size` 保留两个尺寸参数，主版本 0 下调用方都传自身 `sizeof`；不再使用的 `catalog::covers` 删除。
2. **精确命令只有一个泛型查找。** `CommandTable::find_exact` 是模板，经 `ExactCommand<Request>` 取请求的意图种类与规范 `measure`/`encode`。特化写在各自编解码的头文件旁（`command_canonical.h`、`command_approval.h`、`command_reauthorization.h`、`command_task_limit.h`、`command_target.h`、`command_scene.h`），新的所有者命令加一个特化，不改命令表。查找分两段非模板实现：`probe_exact` 按形状、身份、下限与字节容量分类，`stage_exact` 在编码进 scratch 之后与已保存的映射逐字节比较或暂存候选；判断顺序、返回码与暂存规则不变。种类与编解码仍在编译期绑定，不经类型擦除，因此不与 ADR-0051 否决的方案冲突。撤销只读待生效控制、从不暂存，改名为 `find_revoke`，不作为特化。
3. **调用事实对 Task 的影响由 work 层决定。** `work/call_propagation` 提供三条规则：`end_task_call`（终态调用：未知结果持有 Task；COMPLETED 先加步骤；其余结束调用并保留用户暂停）、`reconcile_task_call`（对账后仍未知则保持持有，Task 已不再选这个调用则不动，否则同 `end_task_call`）和 `skip_queued_call`（等待超时的排队调用记 SKIPPED 并结束本次运行，不推进下一个 occurrence）。`CallRuntime` 记录调用事实后在同一 stamp 中调用它们，不再有 `finish_task`。`architecture.json` 声明 work 根目录可包含 `call`；规则只调用 `CallTable`、`TaskTable` 已有的操作。资源等待暂停与恢复时的 Task 持有只改一张表，仍由调用方直接调用。
4. **组合根共享一份默认配置。** `runtime/common/include/ayni/runtime/common/default_config.h` 提供 `ayni::runtime::default_config()`：每个容量取文档的 `_DEFAULT`，可选存储为 0（关闭），允许 0 的上限取默认。它只在 `runtime/` 内使用，不是公开 ABI。Native 用接口库 `ayni_runtime_common`，ESP-IDF 组件以私有包含目录引入。`architecture.json` 的组件规则增加 `kind:instance` 写法，`runtime` 可包含 `runtime:common`，仍不能包含其他 runtime 实例。SimNode 与演示装配随 P2。
5. **文档与草案对齐代码。** `core/README.md` 的模块表按当前代码重写，内存数字改为以 `tools/measure_kernel_memory.c` 的输出为准，不再手写；该工具的默认配置改为完整结构体加 `_DEFAULT`，与 `default_config()` 一致。`AGENTS.md` 删去 `budget/`。草案 `control_v1.h` 删去与 `execution.h` 冲突的目标验证值，改为指向正式类型。

## 后果

- ABI 布局、`AYNI_ABI_MINOR`、存储 schema 与持久记录均不变。旧布局的测试改为拒绝测试：配置与 Host 表的旧布局和任意更短尺寸、Kernel 信息与 step 报告的旧布局都为 `AYNI_INVALID_ARGUMENT`。只有 append/read_prefix 的 Host 表仍可 create、不能 start（C-HOST-015），现在以完整尺寸、其余存储端口为空的表表示。
- `tools/measure_kernel_memory.c` 的 4 个配置在改动前后输出相同的字节数与对齐（默认 216,776 B、最小工作 12,360 B、双 Agent 调用 482,552 B、双 Agent 执行 535,576 B，对齐 8 B，Native 64 位 ASan 构建）。
- P1 起新增的所有者命令在自己的编解码头文件旁加一个 `ExactCommand` 特化，在 `intent_traits` 加一行，在 `Intent` 加请求字段，并在 `intent_rules.cpp` 的 `find_command` 加一个分支；命令表不变。工具调用对 Task 的影响在 `work/call_propagation` 加规则，不进 `CallRuntime`。
- 新增配置字段改 `default_config()`，并同步 `tools/measure_kernel_memory.c` 中的 C 副本（C99 工具不能包含 C++ 头文件）；P2 的 CLI 直接使用 `default_config()`。

## 备选方案

- **保留旧布局兼容，直到 1.0**：两套规则继续并存，P1 的 ABI 0.37 还要为新字段决定适用哪一套；按当前头文件编译的调用方从不受益。用户选择只收当前布局。
- **旧布局一律返回 `AYNI_UNSUPPORTED_VERSION`**：需要保留每个结构体的历史尺寸表才能区分“旧布局”与“错误尺寸”，正是要删除的内容；版本字段位置固定，先比较版本已能告诉其他 minor 的调用方。
- **精确查找改为（种类, 已编码字节）的非模板接口，调用方先编码**：调用方要自备与命令表 scratch 同样大小的缓冲，或者命令表暴露 scratch；两者都把“只有一个候选、插入前不得有其他变更”的不变量分散到调用方。
- **调用传播放在 `work/call/` 并声明 `call → task`**：`CallTable` 是调用事实的属主，让它的目录依赖 Task 会让调用表的单表测试也要建 Task 表；跨表规则与已有 `propagation` 一样放在 work 根目录。
- **默认配置作为公开 API（如 `ayni_config_defaults()`）**：增加 ABI 符号和版本义务，而调用方本就需要按部署调整容量；只在组合根之间共享即可。

## 验证

- `KernelApi.struct_size_below_this_layout_is_invalid_argument`：配置在 `abi_major` 中途、0.2 布局、0.13 布局与 `sizeof - 1`，Host 表在时钟之前、0.2 布局、0.3 布局、只有旧存储 pair 的布局与 `sizeof - 1`，都为 `AYNI_INVALID_ARGUMENT`，内存需求查询同样拒绝且不写输出。`KernelApi.config_version_is_judged_before_its_size`：其他 minor 的短配置与长配置为 `AYNI_UNSUPPORTED_VERSION`，不含 `abi_minor` 的配置为 `AYNI_INVALID_ARGUMENT`。`KernelLifecycle.kernel_info_checks_its_output` 与 `KernelLifecycle.step_rejects_bad_arguments_without_effect` 检查旧布局被拒且不写出；`KernelLifecycle.host_with_only_legacy_storage_cannot_start` 检查 C-HOST-015。
- 命令表、Grant、撤销、审批、重新授权、Task 上限、目标命令与 Scene 的现有测试不改期望值并通过；测试中的花括号请求改为写明类型，撤销查找改用 `find_revoke`。
- 调用的完成、取消、拒绝、恢复为未知、对账与等待超时的现有测试（`kernel_call_test.cpp` 的 `ManagedCall`、`BestEffortCall`、`ShortCallWorkflow` 与 `ManagedExecution` 等）不改期望值并通过。
- `test_check_deps.py`：work 根目录可包含 `call`、仍不能包含未声明的子目录；每个 runtime 实例可包含 `runtime/common`，Host 不能；runtime 之间仍不能互相包含。`check_deps_repository` 通过。
- 完整 ASan/UBSan 套件在本地通过；ESP-IDF 组件的构建由 CI 的完整验证检查。
