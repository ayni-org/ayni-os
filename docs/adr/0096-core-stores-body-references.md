# ADR-0096：正文存储与 Core 只存引用

- **状态**：已接受。部分取代 ADR-0054 中“提议经 `call_tool` 读取”与 ADR-0085、ADR-0090 中消息正文进入 Core 记录和检查点的规定；其余条款不变。
- **日期**：2026-11-02。
- **关联**：ADR-0014、ADR-0038 决策 6、ADR-0054、ADR-0085、ADR-0090、ADR-0095；`include/ayni/bodies.h`、`contracts/storage/records.md`。

## 背景

ADR-0095 让消息链路只受 token 约束，但模型请求、模型回答、对话文本和工具调用列表仍以字节形式进入 Core：调用表按 `call_input_max_bytes`/`call_result_max_bytes` 为每个槽预留输入与结果，邮箱为每个槽预留结果正文，Task 参数和检查点保存对话原文，`AYNI_CALL_TEXT_BYTES_MAX` 与快照的 `call_tool` 回调都假定正文在 Core 内存里。Core 不分配内存（ADR-0014），只能按最大消息预留，这与“按需分配、不按字节预留”的决定冲突。

## 决策

1. **正文存储。** 组合根提供 `ayni_body_store_v1`：`write` 在调用方选择的 scope 内去重并在正文持久前回答 NOT_READY，`read` 按引用复制原文，`release` 幂等。引用 `ayni_body_ref_v1` 是 id、FNV-1a 摘要与长度；摘要用于区分内容和发现错配，不防御刻意碰撞。Native 用 SQLite 会话库，portable 用 Host 外层文件值，模拟、示例与测试用内存实现。
2. **外部字段。** schema 的文本字段可标 `AYNI_SCHEMA_EXTERNAL`，此时值既可内联文本，也可以是 `AYNI_VALUE_BODY` 引用，长度仍受 `max_count` 约束；一个值至多 4 个引用。授权投影与物理投影不能引用外部字段，所以 Core 不读正文也能完成授权。模型请求的 `body` 字段在带正文存储的 Provider 中是外部字段，Provider 自己读取并校验摘要，不符时以效果 NONE 失败，不发出请求。
3. **结果先存后递。** Provider 先把回答（文本，或 `ayni_tool_calls_write` 写出的工具调用列表）写入正文存储，存储回答 OK 后才 post 带 `body` 的结果；邮箱槽只保存固定大小的回执，不再有 `payload_bytes_max`。只有 COMPLETED 的 MODEL 结果可以带引用。快照 `call_result` 返回内联文本或引用；内联工具结果的 `text` 就是列表编码。`call_tool` 删除，原位置保留为恒为空的 `reserved_call_tool`，策略用纯函数 `ayni_tool_calls_read`/`ayni_tool_call_read` 解码。
4. **Core 只存引用。** CALL_STATE 追加结果正文引用，Core schema 升为 9；schema 8 及更早介质只读受限并保留，不迁移（ADR-0038 决策 6）。`call_input/result_max_bytes` 只覆盖结构化参数，由组合根取已注册能力声明的最大值（`runtime::fit_call_bytes`），不再手写余量；`AYNI_CALL_TEXT_BYTES_MAX` 删除。
5. **释放。** Core 不读写正文，只释放。Task 退休或调用内容擦除时，引用进入调用表内的定长队列，标记丢弃它的提交序号；该提交持久后，Core 才通过 `ayni_host_v1.bodies` 调用 `release`。Core 从未持久引用过的正文（被拒绝、过期、同一转换内丢弃、查询或关闭排空收到的正文）立即释放；Core 同步拒绝的 post 不进邮箱，由 Provider 自己释放。重放只重建状态，不重复释放，隐私擦除的重放也不再排队。没有正文存储时队列照常清空，正文归组合根。Native 存储的正文标识不复用（SQLite AUTOINCREMENT），所以迟到的释放不会删到新正文。
6. **孤立正文。** 三种情况会在 Task 与调用 scope 留下无人引用的正文：提交持久与释放之间崩溃（释放队列只在内存里）、策略写入请求正文后调用被拒绝或决策被丢弃。当前三种存储只回收对话 scope，这些正文会留到所属对话删除；按 Core 现存引用在启动时回收是后续工作。
7. **次数设置。** `ayni_config_v1.task_model_call_hard_limit` 原位改为 `max_turns`：新 Task 的模型调用次数，默认 8；所有者可把暂停的 Task 提高到 `AYNI_MAX_TURNS_MAX`（64）。组合根从运行设置 `max_turns` 填写（ADR-0095）。ABI 升为 0.54，主版本 0 只收当前布局，所有调用方重编译（C-VER-006）。

## 后果

Core 的调用表与邮箱大小不再随消息长度变化，超过 200 KiB 的请求和回答能完整往返。代价是组合根必须提供正文存储，Provider 要在工作线程里读写正文；存储故障表现为调用失败或 NOT_READY 等待，不会让 Core 读取不完整的正文。旧状态目录不能直接打开，需要新目录或原二进制回退。

## 备选方案

不采用“Core 保存正文但按需增长”：Core 没有堆，增长只能变成更大的固定预留。不采用“引用里带指针”：引用要能持久化并跨重启、跨进程使用。不采用“由 Provider 在 post 时回读正文交给 Core”：那仍要求 Core 为最大正文预留缓冲。

## 验证

`OpenaiProvider.request_and_answer_over_200_kib_travel_by_reference`、`OpenaiProvider.request_body_that_differs_from_its_reference_is_refused_without_io`、`CallTableTest.retired_result_body_is_released_only_after_its_dropping_commit_is_durable`、`TaskLimitApi.max_turns_sets_initial_limit_without_affecting_steps`；`tests/contract/abi_layout.c` 固定 0.54 布局；Core 无堆检查（`check_deps`）照常通过。正常路径之外的断电、切断与恢复专项本轮没有新增。
