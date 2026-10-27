# 公开 SDK 与接入示例

`ayni/ayni.hpp` 是源码级 C++17 facade：无异常、RTTI，C ABI 仍是接口权威。`initialized<T>()` 初始化 struct_size；`Kernel` 支持自有 aligned arena 或 `create_in` 外部 arena。`submit_raw` 保留 C 的 kind/Body 配对前置条件，不是静态类型映射。

正常收尾是 `drain(正步数, progress)` → `close()`。progress 在 owner 的 step 外推进异步服务；close 在 CONFIGURING/STOPPED 调用创建时提供的 quiesce，确认所有生产者已从最后一次 post 返回，再 destroy/free。hook 必须有界且不抛异常，未就绪返回 BUSY。保留 Host/Provider/context 和外部 arena，直到 close 成功；wrapper 的防御性 arena 保留不能延长它们的生命周期。move 不移动 hook context，也不允许覆盖一个活 Kernel。

四个 main 仅依赖公开头文件。`runtime/examples` 选择具体平台和服务，示例通过公开注册/推进/排空端口接入自己的代码：

| 可执行目标 | 新增代码 | 实际证明 |
|---|---|---|
| `ayni_example_new_hardware` | 模拟触觉按钮 SubscriptionProvider 与规则 | start ACK、当前 press 样本、停止确认、租约/renew/cancel 的公开形状；无真实硬件 |
| `ayni_example_new_model` | 第二模型的消费者策略 | 实际 OpenAI-compatible loopback HTTP，独立 binding/destination、显式 Grant、精确回复和一次请求 |
| `ayni_example_new_strategy` | LocalInput 事件规则 | 注册策略/目标、复制输入、一个 durable Task 完成，不修改 Core |
| `ayni_example_composite_workflow` | 捕获→模型→停止→呈现 | prompt 携带实际 Mock scalar 和来源身份；历史推断不作当前动作证据，停止与通知完成分别核实 |

```sh
# 按 docs/development.md 先设置资源预算和显式构建/测试并行度。
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug -R '^(CppSdk\.|cpp_sdk_|Examples\.)'
bash examples/two_agents_demo.sh build/native/targets/native_cli/ayni
```

双 Agent 脚本仅用 Mock，使用自己创建的临时私有目录并清理；授予演示所需的具体权限，展示文本路由、两 Agent、自主暂停及保留提醒。Native 未可信校时时提醒保持待校时；它不把未呈现当成功。

`new_model` 只使用 numeric-loopback 和进程内 fixture-only key，不读取真实凭据或访问外部模型。可复用协议 helper 在 Provider test_support，产品 CLI 不链接 fixture。独立示例若无法成功 close，会明确失败退出并保留借用至进程结束；这不作为正常关闭证据，也不是 SDK 库的通用退出策略。

CI 编译并运行四个示例和脚本；阶段完整验证另按工作流运行。正常示例不替代完整 E1–E4、故障恢复、真实媒体、硬件停止时限或 Core Alpha。
