# C++ SDK 与公开接入

- **关联**：ADR-0067；MA-020；C-HOST-018、C-VER-002。C ABI 0.45、schema 2 不变。

当前 AP-06 接入及资源映射为 ABI 0.52 / Core schema 8，见 ADR-0087；上方版本说明保留 ADR-0067 的历史范围。

| 编号 | 规范 | 反例 | 验证 |
|---|---|---|---|
| C-SDK-001 | Kernel只拥有一个handle及可选自有arena；重复create拒绝，move清空源，禁止复制/覆盖活目标；外部arena从不由wrapper释放 | move后两对象都destroy同一Kernel，或借用arena被delete | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`、`CppSdk.caller_arena_remains_owned_by_caller_after_successful_close` |
| C-SDK-002 | drain以正步数有界推进，progress在step外；0为INVALID_ARGUMENT。close只在CONFIGURING/STOPPED执行quiesce→destroy→free；quiesce BUSY/失败保留整个Instance | STOPPED后立即free，最后post返回链访问已释放Host | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`、`CppSdk.configuring_destructor_joins_before_releasing_its_owned_arena`；拒绝析构分支只读审查，未新增故障专项 |
| C-SDK-003 | wrapper/示例客户端C++17、无异常/RTTI，C仍兼容C99；raw提交不保证kind/Body类型映射，Core唯一验证；不把STL跨C ABI | C++异常穿过C回调，或wrapper另维护目标状态真相 | CMake的cpp_sdk_without_exceptions_or_rtti及四个example编译目标；`CppSdk.caller_arena_remains_owned_by_caller_after_successful_close`；C99既有检查保持 |
| C-SDK-004 | 四例仅用公开API与窄runtime fixture；具体平台/模型组装在runtime，第三方adapter/policy业务在examples。新模型实际loopback且显式授权，组合等待订阅STOPPED及成功通知 | 示例改Core内部表，或只查descriptor就称模型已运行 | examples/CMakeLists.txt 的四个Examples CTest程序；依赖检查器强制公开边界；实际执行结果在SDK PR |
| C-SDK-005 | owned arena保留不延长外部Host/Provider/代码/context/arena生命；调用方保留它们直到close成功，独立示例失败排空在栈展开前失败退出 | Kernel仍借用外部栈ctx，但示例普通return销毁它 | `CppSdk.owning_arena_move_drain_and_producer_join_preserve_one_handle`；四示例共同finish边界静态审查；未新增故障/恢复专项 |
| C-SDK-006 | 共用软件应用仅用公开 API，Task checkpoint 拥有观察证据；runtime 选择 Host/Provider 并保持统一时钟域。Native/Sim 用同一应用核对真实结构结果、有效样本及正常关闭；Native 生产者 join 后才销毁 Kernel | 换 Host 时复制应用规则、扩展仍收到另一 Host 的时间，或仅看 COMPLETED 就声称求和正确 | `SdkHosts.native_extension_uses_selected_monotonic_clock`；examples/CMakeLists.txt 注册 Native/Sim 受管操作程序，Native 由 tests/integration/sdk_hosts_test.py 核对实际日志与成功退出 |
