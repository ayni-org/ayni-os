# C++ SDK 与公开接入

- **关联**：ADR-0067、ADR-0096；MA-020；C-HOST-018、C-VER-002。C ABI 0.45、schema 2 不变。

当前为 ABI 0.54 / Core schema 9（正文引用见 ADR-0096，通用工作区见 ADR-0090）；AP-06 接入及资源映射见 ADR-0087；上方版本说明保留 ADR-0067 的历史范围。Provider 用 `values.hpp` 的 `ayni::encoded_bytes`/`ayni::text_value_bytes` 测量契约的最大编码，组合根用 `runtime::fit_call_bytes` 取已注册契约的最大值，不手写信封字节。

| 编号 | 规范 | 反例 | 验证 |
|---|---|---|---|
| C-SDK-001 | Kernel只拥有一个handle及可选自有arena；重复create拒绝，move清空源，禁止复制/覆盖活目标；外部arena从不由wrapper释放 | move后两对象都destroy同一Kernel，或借用arena被delete | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`、`CppSdk.caller_arena_remains_owned_by_caller_after_successful_close` |
| C-SDK-002 | drain以正步数有界推进，progress在step外；0为INVALID_ARGUMENT。close只在CONFIGURING/STOPPED执行quiesce→destroy→free；quiesce BUSY/失败保留整个Instance | STOPPED后立即free，最后post返回链访问已释放Host | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`、`CppSdk.configuring_destructor_joins_before_releasing_its_owned_arena`；拒绝析构分支只读审查，未新增故障专项 |
| C-SDK-003 | wrapper/示例客户端C++17、无异常/RTTI，C仍兼容C99；raw提交不保证kind/Body类型映射，Core唯一验证；不把STL跨C ABI | C++异常穿过C回调，或wrapper另维护目标状态真相 | CMake的cpp_sdk_without_exceptions_or_rtti及四个example编译目标；`CppSdk.caller_arena_remains_owned_by_caller_after_successful_close`；C99既有检查保持 |
| C-SDK-004 | 四例仅用公开API与窄runtime fixture；具体平台/模型组装在runtime，第三方adapter/policy业务在examples。新模型实际loopback且显式授权，组合等待订阅STOPPED及成功通知 | 示例改Core内部表，或只查descriptor就称模型已运行 | examples/CMakeLists.txt 的四个Examples CTest程序；依赖检查器强制公开边界；实际执行结果在SDK PR |
| C-SDK-005 | owned arena保留不延长外部Host/Provider/代码/context/arena生命；调用方保留它们直到close成功，独立示例失败排空在栈展开前失败退出 | Kernel仍借用外部栈ctx，但示例普通return销毁它 | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`；四示例共同finish边界静态审查；未新增故障/恢复专项 |
| C-SDK-006 | 共用软件应用仅用公开 API，Task checkpoint 拥有观察证据；runtime 选择 Host/Provider 并保持统一时钟域。Native/Sim 用同一应用核对真实结构结果、有效样本及正常关闭；Native 生产者 join 后才销毁 Kernel | 换 Host 时复制应用规则、扩展仍收到另一 Host 的时间，或仅看 COMPLETED 就声称求和正确 | `SdkHosts.native_extension_uses_selected_monotonic_clock`；examples/CMakeLists.txt 注册 Native/Sim 受管操作程序，Native 由 tests/integration/sdk_hosts_test.py 核对实际日志与成功退出 |
| C-SDK-007 | 纯值/Schema/参数操作及原证明读取显式借用查询大小、对齐的工作区；不足返回 BUFFER_TOO_SMALL，非法布局/对齐拒绝且保留输出。区域仅同步借用，不与输入输出重叠；证明回调使用独立区域做嵌套解析。Kernel owner 区域计入arena，Producer不访问；Core不分配堆、无TLS/全局工作区 | 共享一个全局解码树、回调覆盖仍借用的原Schema、短区域却部分改写输出 | `Workspace.undersized_misaligned_or_unknown_regions_preserve_value_outputs`、`Workspace.independent_regions_normalize_values_on_independent_threads`、`Workspace.callback_refusal_erases_its_lane_and_allows_immediate_reuse`；core_does_not_reference_heap_allocator、workspace_erasure_survives_optimized_dead_storage；既有业务/ToolLoop回归 |
| C-SDK-008 | 独立消费者通过公开源码SDK目标导入同一Core/ToolLoop，不导入Core私有头、内部部署或内部测试fixture；Native真实文件Host与ESP消费者编译相同公共场景，平台组合只选择Host/内存。运行、编译与硬件证据分别记录 | 复制Core到设备工程、消费者依赖根部署/SQLite/CURL、Hello链接代替完整程序 | `portable_independent_normal`；`build_esp.py`及ESP完整CI（编译）；实机未验证 |
| C-SDK-009 | 共同会话桥接在持久draft后提交Goal，Task来源发布持久后写正文，真实文件完成后精确ACK，ACK持久且当前锚点可用才公开正文；每次读取/输入重查Core。Core hold保持原Task供owner处理，其他Agent文件确认继续；未知文件结果、锚点冲突拒绝并保留 | 队列接受代替fsync、旧正文在forget后继续曝光、软上限后重新提交Goal/重复工具、一个Agent hold堵住其他Agent ACK | `portable_independent_normal`中的memory/immediate_privacy_barrier/resume_model_limit及正常重开；冲突/未知只读审查，未开新专项 |
| C-SDK-010 | 单owner、不可重叠的同步策略/codec/value操作可共享按公开query最大值规划的区域，回调嵌套须另区，worker只借独立缓冲；query明确不含Host及Provider自有有界堆；关闭join后才释放 | worker借用scratch、嵌套覆盖正在使用的区域、query被宣称为全部平台内存、STOPPED即释放尚未join的context | `portable_independent_normal`；共享调用链独立审查；ESP heap/stack采集及25%断言仅代码，实机未验证 |
