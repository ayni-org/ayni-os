# ADR-0067：C++ 生命周期封装与公开接入示例

- **状态**：已接受；正常实现、验证与独立审查见 [#89](https://github.com/RobotSe7en/ayni/pull/89)
- **日期**：2026-10-27
- **关联**：P5 / MA-020；ADR-0010、0014、0055、0057、0066；计划 §9.1

## 背景

C ABI 已交付完整正常工作链，应用需要更简明的资源所有权、初始化和排空入口，同时第三方接入应能只使用公开声明。STOPPED 表示 Core 已消费完成，但不能证明外部生产者已经从最后一次 post/Host wake 返回；仅在析构中释放 arena 会越过这一借用边界。

## 决策

1. **源代码封装。** `include/ayni/ayni.hpp` 提供 C++17 Kernel owner、版本化描述符初始化与 raw intent 提交。没有异常、RTTI 或 C ABI 上的 STL；不是独立二进制 C++ ABI。C ABI 仍为 0.45，所有 C 结构/状态校验仍属于 Core。raw 提交不宣称 intent kind 与 Body 的静态类型关系；完整原始 C 接口经借用句柄使用。
2. **arena 所有权。** 可选择 nothrow aligned new 的自有 arena 或调用方提供的 arena；两个路径共用 C create 和失败清理。非空 wrapper 不可被 create 覆盖；move 构造转移完整 handle/arena/hook，清空源；禁止复制及 move assignment，避免替换活所有者。借用 arena 只在成功 close 后归还调用方，从不由 wrapper delete。
3. **有界排空与关闭。** `drain` 用正步数预算，在每个 step 外调用可选 progress；达到可销毁 CONFIGURING/STOPPED 返回 OK，预算不足 BUSY，0 明确 INVALID_ARGUMENT。`close` 只在可销毁状态调用创建时给定的 quiesce proof，然后 destroy/free；quiesce 未就绪或失败保留整个 Instance。hook 必须有界、不抛异常，未就绪返回 BUSY；context 不隐含绑定 wrapper 地址，move 不移动 context。
4. **析构边界。** 析构尝试一次 close，不自动推进异步 drain；拒绝关闭时保留 arena，不假报成功。保留 arena 无法延长调用方 Host、Provider、代码和外部 arena 的生命，调用方必须显式 close 并在失败时保留它们。示例进程的最终 close 失败会在外部栈上下文析构前明确失败退出；SDK 库本身不终止进程。
5. **四个公开示例。** 新硬件为有有限租约的模拟触觉按钮 C SubscriptionProvider，确认 start、发布 press、确认停止后完成；新模型使用实际第二 OpenAI-compatible Provider 的独立 endpoint/destination，通过 numeric-loopback HTTP 验证结果和请求数；新策略以 LocalInput 驱动规则；`composite_workflow` 按当前 Mock 样本→模型历史推断→确认订阅停止→成功通知完成。稳定 step key、原授权/资源/计数继续由 Core 强制，不复制其状态机。
6. **组合根。** `runtime/examples` 只组装具体 Sim Host、共享 DemoProfile 和第二模型协议夹具；示例持有第三方 adapter/policy，并提供公开注册/推进/quiesce 端口，由组合根统一调用。示例源码只可 include 公开 C/C++ API 与窄 `runtime:examples` 公共 fixture，不能 include Core 私有头或其他 runtime。新的业务决定保留在 examples，fixture 不导出可移走的 Kernel owner。
7. **共享与构建边界。** 原 loopback helper 迁到 OpenAI Provider 的 test_support，保留旧测试别名并供示例复用；只在协议测试/示例使用，不进入产品 CLI。静态 Provider 库声明其 public provider_post 的 Core 实现链接依赖，修正纯间接消费者的链接顺序；没有 Core→Provider 反向依赖或私有 Core 头。依赖检查只给特定 ayni.hpp 开放 new/optional，不放宽其余公共 C 头；CI 格式检查覆盖 hpp。
8. **验证入口。** 四个可执行示例及双 Agent Mock 脚本通过 CTest 随普通完整 CI 运行。wrapper/示例客户端按 C++17、无异常/RTTI 编译；ESP-IDF 组合根也包含同一 facade 以验证固定固件工具链可编译，不能推为板级运行。示例只用 Mock 或本地 HTTP，不读真实模型密钥、不连接外部付费服务。

## 后果

普通调用方可以保留 C ABI 的可检查契约并减少生命周期样板。外部生产者的配合仍是必要前提；wrapper 不能替 Host/Provider 证明线程停止。公开 fixture 是正常接入证据，不是用户部署、完整 E4 或跨节点线协议。它的显式 owner grants 只属于示例进程，不改变产品的默认无授权行为。

## 备选方案

- 在析构中无限 drain/join：阻塞不可见且无法报告失败，拒绝。
- 释放 STOPPED arena 后再 join：最后 post 的返回链仍可能访问 Host，拒绝。
- 示例直接调用内部表或自己选择具体 Host：不能证明公开接入与组合根边界，拒绝。
- 另写一套 Mock HTTP/server：已有本地协议 fixture 可共享，选择迁移并保留原测试兼容别名。

## 验证

正常用例验证自有/外部 arena、move、关闭前置条件、quiesce BUSY 后继续、CONFIGURING 析构、无异常/RTTI 编译、四个公开接入示例、双 Agent 脚本和原协议消费者。命令、结果、独立审查及最终适用 CI 集中在 [#89](https://github.com/RobotSe7en/ayni/pull/89)；没有新增故障/恢复、变异专项或真实模型测试。硬件、时限与完整 E1–E4 未据此验收。
