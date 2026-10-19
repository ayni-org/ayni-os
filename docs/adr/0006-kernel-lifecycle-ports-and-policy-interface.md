# ADR-0006：Kernel 生命周期、Host/Provider 边界与策略接口

- **状态**：已接受（设计基线，待实现与验证）
- **日期**：2026-10-18
- **关联**：计划 §2.0、§2.5、§3.2、§3.3、§4.4、§6；`MA-005`；E2 F13、E3 步骤 4

## 背景

- v1.3 的 C API 规定“注册在启动前完成”，却没有启动调用。恢复需要已注册的目录来解释持久化的 ActionRecord 与检查点，又依赖异步存储读取，因此只能隐式发生在前几次 `step` 中。若 Kernel 在幂等映射重建前受理管理端的重试，可能创建重复对象。停机完成也没有可观察的信号。
- 存储与加密同时出现在 Host 服务和 Provider 两侧：若 journal 存储经 Provider 准入，屏障写入可能因预算被拒。
- 策略回调能否在 step 内调用 Kernel API 没有规定，与“不得在 step 内递归回调”相矛盾。

## 决策

1. Kernel 生命周期为 `CONFIGURING → RECOVERING → RUNNING`，异常时进入 `RESTRICTED`，停机为 `DRAINING → STOPPED`。新增 `ayni_start` 与 `ayni_get_lifecycle`；非 `RUNNING` 时 `submit_intent` 返回 `NOT_READY`。保留逐对象的 `register_*`，引用在 `ayni_start` 时统一校验，注册顺序无关；首版不提供运行时注销；可选 `ayni_error_v1` 输出分类原因。
2. Host 是 Kernel 维护自身不变量所用的服务（时钟、持久存储、网络、加密、生命周期），不经 Grant 或预算准入；Provider 是 Agent 经逻辑调用使用的能力实现，经准入、计费与绑定。原 `providers/storage` 改为 `providers/retrieval`；国产密码算法属于 Host 加密实现的选择。
3. 策略与 GoalType 是有界决策函数：输入只读快照与私有检查点，返回有界意图与新的私有检查点，回调内不调用 API、不执行 I/O、不阻塞。

## 后果

- 恢复顺序可以由 API 表达和测试；屏障写入不会被预算拒绝；策略可以确定性重放，E3 的策略替换等价于用同一快照调用另一个函数。
- API 增加两个函数与生命周期事件；快照需设计为窄视图，在 ESP32 上以零拷贝的 const 视图实现，有效期仅限本次调用。

## 备选方案

- **在 `ayni_create` 内恢复**：此时目录为空，无法解释持久记录，否决。
- **以一个嵌套 manifest 一次性注册**：ABI 演进更难；`ayni_start` 时统一校验已消除注册顺序依赖，否决。
- **允许策略回调内调用 API**：带来重入与确定性问题，否决。

## 验证

`MA-005` 给出各生命周期状态下每个 API 的返回值表；E2 F13 验证恢复窗口内的重试；E3 步骤 4 验证策略替换后的确定性重放。
