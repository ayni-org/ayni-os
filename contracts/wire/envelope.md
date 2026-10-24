# 消息信封

- **关联**：计划 §3.1、§3.4、§3.5、§3.6；ADR-0004、ADR-0009、ADR-0030、ADR-0031、ADR-0026；`MA-004`、`MA-007`、`MA-016`、`MA-038`；E2 F9、F13，E3 步骤 2
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
| C-WIRE-004 | `command_id` 的幂等作用域是 `(已认证 principal_id, target_device_id, command_id)`，绑定命令类型与规范化请求身份；旧ordinary命令按原摘要契约兼容，授权命令保存完整canonical并确切比较；相同命令重试返回原对象或原结果；同ID不同身份返回 `AYNI_IDEMPOTENCY_CONFLICT`；映射与业务接受结果同批提交（批次 B1） | 创建提醒的响应丢失，客户端重试后设备上出现两个提醒 | M1 部分实现（Kernel 侧：作用域为会话的 principal 与 `command_id`，`target_device_id` 随管理端协议；摘要覆盖意图种类、`expected_revision` 与正文；重试返回原结果，跨会话与重启；同 ID 不同摘要返回 `IDEMPOTENCY_CONFLICT`；映射与接受结果同批提交，ADR-0026）：`WorkCommands.retried_command_returns_its_first_result`、`WorkCommands.refused_command_is_not_kept`、`WorkCommands.mappings_survive_a_restart_with_their_transitions`；E2 F9；C-COMMIT-005 |
| C-WIRE-005 | 命令映射的保留期不短于该命令允许的重试期限；以主体命令代际/窗口与有界拒绝标记防止过期旧命令被当作新命令；不得仅凭参数相同合并用户确实想创建的两个目标 | 去重记录回收后，一周前滞留的创建命令又创建了一个提醒 | M1 部分实现（Kernel 侧：每个 principal 保留最近 `command_capacity` 个映射，让位时把下限提高到让位的 `command_id`，不大于下限的未映射命令以 `EXPIRED` 拒绝；下限随映射记录持久；参数相同而 `command_id` 不同的命令各自应用，ADR-0026）：`WorkCommands.full_table_drops_the_smallest_id_and_raises_the_floor`、`WorkCommands.floor_survives_a_restart_with_more_room`；重试期限与命令代际随管理端协议（`MA-038`）；E2 F9（过期命令） |
| C-WIRE-006 | `operation_id` 的作用域是 `(已认证调用主体, 控制域 authority-owner, operation_id)`，绑定能力 ID/版本与规范化参数摘要；同 ID 不同摘要拒绝；调用方在屏障前、接收方在持久接受时保存映射 | 接收方只按 `operation_id` 去重，不同主体的同号请求被当作重复返回了别人的结果 | `MA-038`；`MA-016`；C-ID-005 |
| C-WIRE-007 | 接收结果按上表区分；客户端没有收到响应不能推定未执行，应以同一幂等键查询或重试；协议不宣称通用的“恰好执行一次” | 客户端超时后以新 `operation_id` 重发转向 | E2 F6；`MA-016` |
| C-WIRE-008 | 查询结果按上表区分；`NOT_ACCEPTED` 只在接收方于同一次提交中持久封禁该 ID 时返回；查不到记录本身不推断未执行 | 接收方回收记录后对旧 ID 返回 `NOT_ACCEPTED`，调用方据此改用其他设备执行 | `MA-038`（查询后原消息迟到）；C-CALL-010 |
| C-WIRE-009 | `ttl_ms` 是首次接收后的执行预算，重试不续期，也不证明传输前的新鲜度；副作用请求首次执行须携带接收方在该会话中最近下发的 `receiver_tick`，超出新鲜窗口返回 `AYNI_STALE`，调用方以同一 `operation_id` 携带新 tick 重试；无法验证时拒绝 | 在网络中滞留 10 分钟的转向请求，以“收到后 1.5 秒内执行”为由被执行 | `MA-038`（首次到达长延迟）；`MA-016` |
| C-WIRE-010 | ObjectRef 至少包含 `origin_device_id`、`kind`、`id`、`revision`；接收方按认证来源解析并检查读取、传输、用途权限与有效性；引用本身不携带权限 | 持有一个观察引用就能读取对应的原始图片 | `MA-016`；E1 步骤 1 |
| C-WIRE-011 | 普通控制 JSON 不内嵌 base64 大文件；媒体经独立受控数据通道传输，引用绑定主体、用途、有效期、大小与内容类型，过期、取消与重启时回收；不接受任意 URL 或文件路径 | 控制消息中的 `"image": "file:///etc/..."` 被设备读取并上传 | `MA-015`；C-PRIM-005 |
| C-WIRE-012 | 请求身份按每种命令/能力的契约所列领域字段规范化；字段顺序无关，仅按该契约补缺省或换单位，未列入字段不参与，不对任意序列化字节取哈希。Grant创建保留原始scope省略、类型/单位严格匹配，其身份不依赖当前catalog；授权展开范围另存 | 同一命令的 JSON 字段顺序不同，被判为 `IDEMPOTENCY_CONFLICT` | ADR-0009 选型后的摘要测试；C-VER-032 |
| C-WIRE-013 | 事件订阅带游标；重连按游标补有限历史；超出保留范围时明确返回 gap 并要求状态查询，不静默跳过 | 网页离线一天后重连，中间的事件被跳过且界面不提示 | `MA-007`；E1 步骤 10 |
| C-WIRE-014 | 每类领域意图只有一个领域类型与一个语义校验器；本地类型化 API 与网络解码器构造同一类型并共用校验器 | 管理 API 与 C API 各自校验提醒时间，规则不一致 | `MA-004`（C ABI 解码为 `core/kernel/intent/intent.h` 的领域类型 `Intent`，格式校验 `check_form` 与语义校验留给网络解码器共用；网络解码器随 `MA-038`）；`MA-005`、`MA-007` 往返测试；C-VER-012 |
| C-WIRE-015 | MCP、A2A 等外部协议适配列出状态、错误、取消、重试与对象引用的映射；外部工具注解不直接变成可信能力属性；远端不保证的去重或查询不由适配层伪造；无法保留必要语义时返回 `AYNI_UNSUPPORTED` | 适配层把 MCP 工具的 `idempotentHint` 当作服务端去重保证 | `MA-030` |
| C-WIRE-016 | 会话重连不改变 `command_id` 的幂等作用域：作用域只含已认证主体、目标设备与 `command_id`，不含会话或连接标识 | 重连后同一 `command_id` 被当作新命令，创建了第二个提醒 | E2 F9（响应丢失后重连重试） |

## 授权命令的确切身份（MA-006 C，已实现并完成 Native 验证）

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-WIRE-017 | Grant命令的完整有界canonical请求与对象/结果同批保存；FNV仅索引，最后比较length+bytes。原始scope只做独立形状校验和字段/集合排序去重，保留省略，不查当前catalog/subject/parent；已映射命令先返回原结果或冲突，不因当前依赖失效而重新应用 | 相同hash异正文扩大权限；升级后目录缺失令旧命令重新创建；省略scope随新目录补全成不同身份 | M1 已实现（创建命令完整身份与同批结果）：`CommandCanonical.sorting_and_deduplication_keep_omission_distinct_from_explicit_constraints`、`CommandTable.a_digest_collision_does_not_replace_exact_byte_equality`、`GrantReplay.a_valid_but_expanded_create_scope_cannot_disagree_with_its_exact_command`；ADR-0030；C-COMMIT-005 |

原始身份含kind、expected_revision、capability ID/version、subject、parent、flags、lifetime、duration和原始scope；Session/当前时间/计算deadline/新Grant ID不参与。省略与显式全域不同，token顺序与重复等价。CommandTable拥有每槽canonical字节及有界scratch；旧ordinary v1映射按原契约兼容，auth不得回退成只有digest。窗口floor和原结果仍有界保留，对象安全回收不授权命令重建。

## 撤销身份与本机回执（MA-006 D，已实现并完成 Native 场景验证）

以下是本机 C ABI 的语义，管理网络协议仍待 `MA-007`/`MA-038`。ABI 0.19 的 result/receipt 统一使用 PENDING/APPLIED/DURABLE；PENDING effect=NONE、结果引用只带目标 Grant ID且revision0、seq/boot0；APPLIED 带本次应用 boot/seq及明确 effect；DURABLE 保留原结果/seq，applied_boot_epoch=0。NO_CHANGE/TARGET_GONE 是持久 no-op，不能据此推断 Provider 或物理动作已停止。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-WIRE-018 | kind10 canonical 独立固定16 B：小端u32 kind、u32原expected_revision、u64 target；kind9格式1不变；所有kind先查pending再查同一窗口/floor，授权命令最终length+bytes匹配、ordinary保留旧摘要契约，pending身份低于新floor仍有效，拒绝不占命令ID | ordinary同ID绕过pending；hash碰撞被当原命令；撤销因floor提升永远不能发布 | M1 已实现并通过 Native 场景：`CommandRevoke.canonical_has_a_fixed_independent_little_endian_oracle`、`RevokeApi.pending_identity_precedes_every_implemented_kind_and_later_window_floor`、`PendingRevoke.full_revoke_identity_still_wins_when_the_window_digest_collides`；最终交付见 ADR-0031 |
| C-WIRE-019 | 可信Host以已认证principal/command查询receipt，不依赖旧Session；pending优先可在恢复/受限诊断，普通结果只在完整恢复后暴露；先映射再floor，未映射低于floor为EXPIRED否则NOT_FOUND；查询失败不改caller输出 | 新Session无法确认旧撤销；部分重放窗口误报完整结果；跨owner查询结果 | M1 已实现并通过 Native 场景：`RevokeApi.applied_and_durable_receipts_keep_original_result_across_a_new_session`、`RevokeApi.receipt_query_errors_leave_caller_owned_output_unchanged`、`RevokeApi.ordinary_receipt_floor_and_bad_query_shapes_preserve_the_output`、`RevokeApi.receipt_window_is_not_exposed_before_initial_recovery_finishes`；最终交付见 ADR-0031；C-AUTH-025 |

## 显式重新授权命令身份（MA-006 F4）

本条款约束 C ABI 管理入口及内部持久 canonical。`GOAL_REAUTHORIZE=12` 的请求为 `ayni_goal_reauthorize_v1`：完整原创建回显、Goal ID、预期授权事实提交序号；外层带非零 command ID 及 Goal expected revision。当前持久 owner 的 MANAGEMENT Session 先校验，随后先查完整命令身份，再查当前对象和目录。已终结或回收目标仍可精确重试，返回原 object/revision/commit_seq；变化的任一 CAS 或回显冲突，不重新换绑、跳过触发或采样水位。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-WIRE-020 | 重新授权原始身份含完整回显及双 CAS；内部 canonical 固定前缀为 kind/revision（u32）、Goal ID/授权 CAS（u64），再接共享原创建 echo；重试不依赖当前目标或目录，字节失败不发布部分解码 | 回收后同命令被新受理；变更旧 CAS 仍算精确重试；截断使输出保留半份新回显 | `GoalReauthorization.exact_retry_survives_goal_reclamation_and_changed_echo_conflicts`、`GoalReauthorizationCodec.exact_identity_round_trip_and_all_truncations_are_atomic`、`GoalReauthorization.requires_management_and_durable_existing_authority`；ADR-0038 |
