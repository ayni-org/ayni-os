# 消息信封

- **关联**：计划 §3.1、§3.4、§3.5、§3.6；ADR-0004、ADR-0009；`MA-007`、`MA-016`、`MA-038`；E2 F9、F13，E3 步骤 2
- **属主**：`kernel`（受管入口：解码 → 校验 → 分派，命令幂等映射）；协议适配层（编解码与传输）
- **状态**：语义在 M0 定义；M1 以多节点确定性仿真与 FakeTransport 验证（`MA-038`）；M2 实现局域网管理 API（`MA-007`）；M5 实现真实认证与设备间调用（`MA-016`）

## 信封字段

| 字段 | 必需 | 规则 |
|---|---|---|
| `protocol_major`、`protocol_minor` | 是 | C-VER-010 |
| `message_id` | 是 | 单次传输的 ID；重试可以改变，不承担业务幂等（C-ID-007） |
| `kind` | 是 | 消息种类，如 `goal.create`、`capability.invoke`、`capability.query`、`event.subscribe` |
| `trace_id` | 否 | 追踪关联，不参与任何判定 |
| `sender_device_id`、`target_device_id` | 是 | 发送与目标设备；发送方身份以已认证连接为准，字段只用于路由与一致性检查 |
| `command_id` | 管理端持久变更命令 | 管理命令的幂等键（C-WIRE-003、C-WIRE-004） |
| `operation_id` | `capability.invoke` | 设备间调用的幂等键（C-WIRE-003、C-WIRE-006） |
| `principal_ref`、`grant_ref`、`binding_id` | 按 `kind` | 只是引用，接收方以本机记录校验（C-AUTH-012） |
| `agent_id`、`instance_id`、`owner_device_id` | 按 `kind` | 调用主体与固定 owner；远程调用不转移任务归属（C-LIFE-020） |
| `goal_id`、`task_id`、`task_revision` | 按 `kind` | 关联的目标与任务 |
| `ttl_ms` | 副作用请求 | 首次接收后的执行预算（C-WIRE-009） |
| `freshness` | 副作用请求 | `session_id` 与接收方下发的 `receiver_tick`（C-WIRE-009） |
| `payload` | 是 | 按 `kind` 的领域类型；Schema 见 `domain/` |

64 位整数字段（ID、计数、序号、`receiver_tick`）在 JSON 中以十进制字符串表示（C-JSON-004）。

## 接收结果与查询结果

| 接收结果 | 含义 |
|---|---|
| `RECEIVED` | 已校验接收，可能尚未持久化 |
| `DURABLY_ACCEPTED` | 已承担持久处理责任（`durable_seq` 覆盖接受记录） |
| `IN_PROGRESS` | 执行中 |
| `COMPLETED` | 结果可查询 |
| `RESULT_UNKNOWN` | 已越过可能派发边界，结果无法确定 |

| 查询结果 | 含义 |
|---|---|
| `FOUND` | 附当前状态 |
| `EXPIRED_UNKNOWN` | 低于去重低水位，结果未知 |
| `NOT_ACCEPTED` | 接收方已在同一次提交中持久封禁该 ID，迟到的原消息会被拒绝；这是权威的未执行结论 |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-WIRE-001 | 每条消息携带上表的必需字段；缺少必需字段、`kind` 未知或必需功能未知时拒绝，返回结构化错误 | 缺少 `target_device_id` 的消息被路由到默认设备执行 | `MA-007` 解析测试；C-VER-010 |
| C-WIRE-002 | 局域网管理传输的每个请求都经认证；停止入口同时连接独立的本地停止路径；管理入口不对公网开放 | 未认证的局域网请求可以查询记忆 | `MA-007`；第 10.6 节「安全接入」 |
| C-WIRE-003 | 每条消息只携带一种幂等键：管理端持久变更命令携带 `command_id`，`capability.invoke` 携带 `operation_id`；两者同时出现或应有而缺失时以 `AYNI_INVALID_ARGUMENT` 拒绝 | 一条 `capability.invoke` 同时带 `command_id` 与 `operation_id`，两个去重表给出不同结论 | `MA-007`、`MA-016` 解析测试；C-ID-008 |
| C-WIRE-004 | `command_id` 的幂等作用域是 `(已认证 principal_id, target_device_id, command_id)`，绑定命令类型与规范化参数摘要；相同命令重试返回原对象或原结果；同 ID 不同摘要返回 `AYNI_IDEMPOTENCY_CONFLICT`；映射与业务接受结果同批提交（批次 B1） | 创建提醒的响应丢失，客户端重试后设备上出现两个提醒 | E2 F9；C-COMMIT-005 |
| C-WIRE-005 | 命令映射的保留期不短于该命令允许的重试期限；以主体命令代际/窗口与有界拒绝标记防止过期旧命令被当作新命令；不得仅凭参数相同合并用户确实想创建的两个目标 | 去重记录回收后，一周前滞留的创建命令又创建了一个提醒 | E2 F9（过期命令） |
| C-WIRE-006 | `operation_id` 的作用域是 `(已认证调用主体, 控制域 authority-owner, operation_id)`，绑定能力 ID/版本与规范化参数摘要；同 ID 不同摘要拒绝；调用方在屏障前、接收方在持久接受时保存映射 | 接收方只按 `operation_id` 去重，不同主体的同号请求被当作重复返回了别人的结果 | `MA-038`；`MA-016`；C-ID-005 |
| C-WIRE-007 | 接收结果按上表区分；客户端没有收到响应不能推定未执行，应以同一幂等键查询或重试；协议不宣称通用的“恰好执行一次” | 客户端超时后以新 `operation_id` 重发转向 | E2 F6；`MA-016` |
| C-WIRE-008 | 查询结果按上表区分；`NOT_ACCEPTED` 只在接收方于同一次提交中持久封禁该 ID 时返回；查不到记录本身不推断未执行 | 接收方回收记录后对旧 ID 返回 `NOT_ACCEPTED`，调用方据此改用其他设备执行 | `MA-038`（查询后原消息迟到）；C-CALL-010 |
| C-WIRE-009 | `ttl_ms` 是首次接收后的执行预算，重试不续期，也不证明传输前的新鲜度；副作用请求首次执行须携带接收方在该会话中最近下发的 `receiver_tick`，超出新鲜窗口返回 `AYNI_STALE`，调用方以同一 `operation_id` 携带新 tick 重试；无法验证时拒绝 | 在网络中滞留 10 分钟的转向请求，以“收到后 1.5 秒内执行”为由被执行 | `MA-038`（首次到达长延迟）；`MA-016` |
| C-WIRE-010 | ObjectRef 至少包含 `origin_device_id`、`kind`、`id`、`revision`；接收方按认证来源解析并检查读取、传输、用途权限与有效性；引用本身不携带权限 | 持有一个观察引用就能读取对应的原始图片 | `MA-016`；E1 步骤 1 |
| C-WIRE-011 | 普通控制 JSON 不内嵌 base64 大文件；媒体经独立受控数据通道传输，引用绑定主体、用途、有效期、大小与内容类型，过期、取消与重启时回收；不接受任意 URL 或文件路径 | 控制消息中的 `"image": "file:///etc/..."` 被设备读取并上传 | `MA-015`；C-PRIM-005 |
| C-WIRE-012 | 参数摘要按每种命令或能力的契约列出的领域字段计算：字段顺序无关、缺省值先补齐、单位先换算到声明单位、未列入的字段不参与；不对任何序列化字节取哈希 | 同一命令的 JSON 字段顺序不同，被判为 `IDEMPOTENCY_CONFLICT` | ADR-0009 选型后的摘要测试；C-VER-032 |
| C-WIRE-013 | 事件订阅带游标；重连按游标补有限历史；超出保留范围时明确返回 gap 并要求状态查询，不静默跳过 | 网页离线一天后重连，中间的事件被跳过且界面不提示 | `MA-007`；E1 步骤 10 |
| C-WIRE-014 | 每类领域意图只有一个领域类型与一个语义校验器；本地类型化 API 与网络解码器构造同一类型并共用校验器 | 管理 API 与 C API 各自校验提醒时间，规则不一致 | `MA-005`、`MA-007` 往返测试；C-VER-012 |
| C-WIRE-015 | MCP、A2A 等外部协议适配列出状态、错误、取消、重试与对象引用的映射；外部工具注解不直接变成可信能力属性；远端不保证的去重或查询不由适配层伪造；无法保留必要语义时返回 `AYNI_UNSUPPORTED` | 适配层把 MCP 工具的 `idempotentHint` 当作服务端去重保证 | `MA-030` |
| C-WIRE-016 | 会话重连不改变 `command_id` 的幂等作用域：作用域只含已认证主体、目标设备与 `command_id`，不含会话或连接标识 | 重连后同一 `command_id` 被当作新命令，创建了第二个提醒 | E2 F9（响应丢失后重连重试） |
