# 公开 SDK 与接入示例

这些是公开机制的接入与应用样例，不要求每个宿主提供相同设备或业务。基础能力先通过 Native/Sim 与软件实现验证，再按[基础能力与实验计划](../docs/architecture/foundation-and-experiment-plan.md)开展 ESP32-S3、Linux 等实际平台实验；`new_hardware` 示例不表示所有部署都须安装外设。

`ayni/ayni.hpp` 是源码级 C++17 facade：无异常、RTTI，C ABI 仍是接口权威。`initialized<T>()` 初始化 struct_size；`Kernel` 支持自有 aligned arena 或 `create_in` 外部 arena。`submit_raw` 保留 C 的 kind/Body 配对前置条件，不是静态类型映射。

正常收尾是 `drain(正步数, progress)` → `close()`。progress 在 owner 的 step 外推进异步服务；close 在 CONFIGURING/STOPPED 调用创建时提供的 quiesce，确认所有生产者已从最后一次 post 返回，再 destroy/free。hook 必须有界且不抛异常，未就绪返回 BUSY。保留 Host/Provider/context 和外部 arena，直到 close 成功；wrapper 的防御性 arena 保留不能延长它们的生命周期。move 不移动 hook context，也不允许覆盖一个活 Kernel。

五个 main 仅依赖公开头文件。AP-06 的受管操作工作流位于 `applications/software_workflow`，由 runtime 选择并注册，两个 Host 使用同一应用；一字节 Task checkpoint 保存观察证据，求和检查真实结构结果。`runtime/examples` 选择具体平台和服务，示例通过公开注册/推进/排空端口接入自己的代码：

| 可执行目标 | 新增代码 | 实际证明 |
|---|---|---|
| `ayni_example_new_hardware` | 模拟触觉按钮 SubscriptionProvider 与规则 | start ACK、当前 press 样本、停止确认、租约/renew/cancel 的公开形状；无真实硬件 |
| `ayni_example_new_model` | 第二模型的消费者策略 | 实际 OpenAI-compatible loopback HTTP，独立 binding/destination、显式 Grant、精确回复和一次请求 |
| `ayni_example_new_strategy` | LocalInput 事件规则 | 注册策略/目标、复制输入、一个 durable Task 完成，不修改 Core |
| `ayni_example_composite_workflow` | 捕获→模型→停止→呈现 | prompt 携带实际 Mock scalar 和来源身份；历史推断不作当前动作证据，停止与通知完成分别核实 |

| `ayni_example_managed_operations` | 可取消分块求和与 I64 软件流 | 同 Task action/订阅并存，读取当前前沿、有限等待、真实结果与 close 终态；只用公开接口 |

```sh
# 按 docs/development.md 先设置资源预算和显式构建/测试并行度。
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug -R '^(CppSdk\.|cpp_sdk_|Examples\.)'
bash examples/assistant_demo.sh build/native/targets/native_cli/ayni
```

助理脚本仅用 Mock，使用自己创建的临时私有目录并清理；授予演示所需的具体权限，展示单一 ToolLoop 助理对话（含 `check` 作为普通文本）、自主检查/观察、“停”的确定性停止及保留提醒（ADR-0072）。Native 未可信校时时提醒保持待校时；它不把未呈现当成功。

`new_model` 只使用 numeric-loopback 和进程内 fixture-only key，不读取真实凭据或访问外部模型。可复用协议 helper 在 Provider test_support，产品 CLI 不链接 fixture。独立示例若无法成功 close，会明确失败退出并保留借用至进程结束；这不作为正常关闭证据，也不是 SDK 库的通用退出策略。

CI 编译并运行五个示例和脚本；阶段完整验证另按工作流运行。正常示例不替代完整 E1–E4、故障恢复、真实媒体、硬件停止时限或 Core Alpha。


`providers/software` 是平台无关的协作式示例 Provider：每次 `advance()` 每槽最多256次加法或一次事件/控制/终态投递，必须在 Kernel step 外推进。无 worker、设备或网络；configure 一次性有界分配，接受后复制请求，成功 terminal post 后归还借用，close 在 pending 非零时返回 BUSY。事件保留原采集时刻，满队列不伪造新鲜值。

AP-06 当前 ABI 0.52 / Core schema 8。`ayni_example_managed_operations` 无参数运行 Sim；传入已经存在的私有空目录运行 Native 异步文件存储。`Examples.managed_operations_native` 自动创建临时目录、核对成功及实际日志，成功关闭后清理。两者不使用真实模型或设备，不代表后续 Linux 系统集成实验。扩展 `advance` 接收所选 Host 的同一单调时钟域。

同能力的多实例通过 Binding 精确选择；`ayni_binding_resource_v1` 将逻辑需求映射为各实例实际资源（省略为恒等），不改变单位/其他执行语义，不能合并需求。完整示例及验证见 `tests/unit/binding_instances_test.cpp` 和 [本包设计](../docs/architecture/ap06-ap04-foundation-design.md)。定义、说明、实现和部署配置分别归现有接口，不增加模型 codec 的业务分支。
