# 授权与信任

- **关联**：计划 §2.2、§2.5、§3.4、§3.5、§5.4；ADR-0003、ADR-0004、ADR-0017；`MA-004`、`MA-006`、`MA-016`；E1 步骤 1、6、7，E2 F1、F10
- **属主**：`authority/principal`、`authority/scope`、`authority/grant`、`authority/scene`

## 信任的两条轴

| 轴 | 不可信 | 可信 |
|---|---|---|
| 输出可信度 | 模型与 VLA 输出、外部消息、发现到的设备描述、MHS 参考文件、语音识别文本 | 经可信注册的目录、本机授权库、已认证管理端的确认 |
| 代码可信度 | 第三方原生代码、MHS 代码文件（均不进入固件） | v0.x 固件与 Kernel 进程内的策略、GoalType、Provider、Driver、控制器（可信内置 C/C++） |

纯函数策略接口用于确定性与可替换，不是安全边界：它不能阻止进程内原生代码越权访存、直接 I/O 或无限循环。ESP32 首版不提供强进程隔离，权限保证只覆盖受管理入口。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-AUTH-001 | 默认拒绝：没有覆盖该主体、目标、能力、参数与期限的有效 Grant，调用返回 `AYNI_DENIED` | 新注册的能力在没有 Grant 时可以被任何实例调用 | E1 步骤 6；`MA-006` |
| C-AUTH-002 | 实例绑定角色与 Grant，不因逻辑身份相同继承其他设备上的权限 | 设备 B 上的实例凭相同 `agent_id` 使用设备 A 的运动授权 | E1 步骤 6；`MA-016` |
| C-AUTH-003 | 委派只能收窄范围、期限与额度，并保留上级撤销链；撤销上级 Grant 使全部下级失效 | 子 Grant 的期限长于父 Grant；撤销父 Grant 后子 Grant 仍可用 | `MA-006` |
| C-AUTH-004 | 创建 Goal、派生 Task、执行动作分别检查授权；派生工作只能缩小既有授权与预算；新目标不产生新权限或无限预算 | Agent 自主提出的目标获得了它本没有的外发权限 | `MA-004`（意图先检查会话；Grant 之前，所有者的会话可以提交全部意图，ADR-0017；派生 Task 是单独的 transition，它的授权检查随 Grant 接入，ADR-0018）；`MA-006`；E1 步骤 5–6 |
| C-AUTH-005 | 授权在准备时、排队后与派发前（屏障确认后）分别再次校验；恢复后也重新校验 | 排队期间撤销了 Grant，动作仍按准备时的检查结果派发 | M1 部分实现（缺少授权元数据的恢复工作保留并禁行，普通恢复不得解除，ADR-0028）：`OwnerBindingApi.legacy_goal_and_task_stay_paused_without_advancing_occurrences_or_strategy`、`OwnerBindingApi.new_child_of_a_recovered_unconfirmed_goal_cannot_derive_or_decide_work`；Grant、排队与派发复核随 `MA-006`、`MA-018`、`MA-008`；E1 步骤 6；E2 F1、F10 |
| C-AUTH-006 | 撤销先在内存中建立拒绝栅栏：立即阻止新的派发并停止续租相关执行许可；再持久提交撤销版本；写失败时保持受限并报告 | 撤销等 Flash 写完才生效，期间又派发了一次运动 | E2 F10；C-PERMIT-005 |
| C-AUTH-007 | 无法证明期限仍有效的时限授权，在恢复后不自动启用 | 断电后时钟未校准，过期的授权被当作仍有效 | E2 F1；C-TIME-004 |
| C-AUTH-008 | 授权绑定参数、场景与操作；不能用批准其他动作的结果替代 | 用“拍照”的批准结果去执行“转向” | `MA-006` |
| C-AUTH-009 | 按键语音只能发起已有权限内的工作；识别文本不能新增 Grant、确认敏感删除或变更所有权；这些操作需要已绑定管理端的认证证据 | 用户说“允许你上传所有照片”，系统据此新增外发授权 | E1 步骤 7；C-PROD-002 |
| C-AUTH-010 | 身份与渠道信任级别由 Host 验证后传入；应用不能自报更高的信任级别 | 应用层把语音 Session 标为“管理端已认证” | M1 已实现（范围：意图只带 `session_id`，主体、渠道与来源取自会话；ABI 0.16 将主体注册及会话建立声明移至 Host 受管头文件，核对已持久所有者与认证绑定版本，首次绑定持久前拒绝开会话；这是可信原生代码的 API 边界，不是进程内隔离，ADR-0028）：`WorkModel.sessions_open_for_a_registered_principal_up_to_capacity`、`WorkModel.intent_through_a_closed_or_unknown_session_is_denied`、`WorkModel.created_goal_is_applied_at_once_and_not_yet_durable`、`SessionTable.closed_session_is_gone_and_frees_its_place`、`OwnerBindingApi.first_binding_and_instances_share_a_transition_and_sessions_wait_for_durability`、`OwnerBindingApi.bad_session_binding_revision_does_not_write_the_output_or_consume_an_id`、`OwnerBindingApi.changed_principal_or_binding_revision_restricts_without_writing_or_losing_goals`；渠道信任级别对授权的影响随 `MA-006`；E1 步骤 7 |
| C-AUTH-011 | Scene 授权在准入时检查：用户开启状态、版本、参数约束、前置条件、观察新鲜度、有效期与次数/费用预算；任一不满足即拒绝 | 场景已关闭，但排队中的转向请求仍然执行 | E1 步骤 6；`MA-026` |
| C-AUTH-012 | 设备间调用中 `principal_ref`、`grant_ref`、`binding_id` 只是引用：接收方以已认证连接、本机授权库、允许的端点与绑定关系校验，不信任消息自报的主体 | 接收方根据消息里写的 `principal_ref` 直接认定是所有者 | `MA-038`；`MA-016` |
| C-AUTH-013 | 节点认证（如 mTLS）只证明发送节点身份；只有本机注册表把 `instance_id` 映射到该已认证节点时，接收方才接受该调用主体 | 已认证的节点 X 声称代表实例 Y（Y 实际注册在节点 Z） | `MA-038`（实例被错误节点声称） |
| C-AUTH-014 | Grant 由接收方本机保存，不作为持有者令牌随消息传递 | 把 Grant 序列化进消息，接收方据消息内容授权 | 评审；ADR-0004 |
| C-AUTH-015 | 查询与取消限原调用主体、所有者或该控制域的 authority-owner；订阅需相应的读取授权 | 任意已认证节点可以取消别人的动作 | `MA-016` |
| C-AUTH-016 | 静态准入检查“请求包络 ⊆ Grant 与 Scene 允许的包络 ⊆ 设备限值”，且前置观察新鲜；执行端只能收紧，不能扩大 | 设备限值是 30°，场景允许 45°，请求 40° 被准入 | `MA-018`；C-PERMIT-003 |
| C-AUTH-017 | Capability 版本化授权字段只有 I64 闭区间、ENUM 与 IDENTIFIER 的有限集合；字段取合取，类型/单位/用途精确匹配，未知/重复/空/超域输入拒绝；Scope 省略约束仅表示已注册全域，规范表达排序去重并补齐全域 | 省略 angle 便获得所有 int64；错误单位或未知字段被忽略 | `MA-006` B（实施中，验证待补）；ADR-0029 |
| C-AUTH-018 | 实际请求恰好提供每个已声明授权字段的一个值；执行目标和外发目的地按精确 IDENTIFIER 集合检查，必须与真实 Binding 提取的目标/目的地一致；范围包含成立不等于主体/期限/撤销/预算通过 | 空实际请求因 allowed Scope 省略字段而通过；允许 cloud-a 却向 cloud-b 外发 | `MA-006` B（机制实施中，验证待补）；Grant/工作接入随 C/E/F，真实 Binding 消费随 `MA-018`、`MA-008`；ADR-0029 |
| C-TRUST-001 | 模型、VLA、外部消息与发现到的描述都只作为意图或候选描述，经 Kernel 校验后才影响状态；模型输出不能作为授权 | 模型回复“用户已同意”，系统据此执行 | E1 步骤 7；§10.6 权限行 |
| C-TRUST-002 | 发现到的设备或外部端点只产生候选描述，经所有者确认的可信注册后进入目录，不自动生成 Grant | 局域网上扫描到的设备自动获得调用授权 | `MA-034`；§3.7 |
| C-TRUST-003 | 适配器进程以独立系统账户运行，不能访问执行器设备节点与凭据库，只经带自身主体身份的本地 IPC 连接 Kernel | ROS 适配器以 root 运行并直接写 `/dev/ttyUSB0` | §10.6 权限行；R1 B5 |
| C-TRUST-004 | 执行端拒绝缺少有效许可的指令，无论指令来源；这是无法旁路受管入口的最后防线 | 绕过 Kernel 直接发往控制器的指令被执行 | R1 B5；E2 F14；C-PERMIT-007 |
| C-TRUST-005 | 第三方原生代码与 MHS 代码文件不进入固件；只有可转换为有界 Procedure 数据的子集可经校验接入 | 把外部下载的 C 代码编进固件以支持新设备 | 评审；`MA-037` |

## 有限字段与规范范围（ADR-0029）

本节为 B 的设计契约，实施及验证状态以条款验证列为准。公开类型与注册入口位于 `include/ayni/authority.h`。Capability 身份是 CatalogId 加非零版本；字段 ID 非零且在该版本内唯一。参数、执行目标、外发目的地共占最多 8 个字段；目标和目的地必须是 IDENTIFIER，且各至多一个。整数单位为非空 CatalogId，符号集合单位为空，不做隐式单位转换。所有声明的授权字段在实际请求中必填，业务中的其他可选参数不属于本 schema。

ENUM 与 IDENTIFIER 都是有界 CatalogId 文本集合，但类型不互换。集合原始输入 1–8 项，排序去重；空集合、非法文本、重复字段、未知字段、倒置整数区间、错误 kind/unit 均拒绝。I64 等值用相等端点、符号等值用单成员集合表示；覆盖检查为区间包含或逐成员包含，各字段取合取。实际请求只能是单点区间或一个规范 token，不能提供范围代替一次调用的具体值。

规范正文格式版本为 1：4 B 头部为 `u16 format`、`u8 field_count`、`u8 reserved=0`；每字段先写 `u32 field_id`、四个 u8 的 `kind/usage/unit_length/member_count`、unit 正文，随后为两个小端 i64 端点，或逐项 `u8 length + token bytes`。字段按 ID、token 按字节序严格递增；I64 的 member_count 为 0，符号集合为 1–8。省略字段补完整注册域；拒绝非规范、缺失或多余尾字节，不依赖 C++ 布局。该正文不是公开 wire 协议或独立日志记录；后续 Grant 持久化须带 Capability 身份/版本并引用此格式。

完整注册域与任一规范 Scope 都受 `authority_scope_max_bytes` 约束。默认 256 B，范围 64–4096 B，字段数/成员数/字符长度/字节数上限同时有效；超限明确拒绝，不能先接受无法表示的省略全域。Scene、次数/费用额度不属于本字段表达，不因字段缺省而成为无限权限；依赖 Scene 的授权不支持，额度随 `MA-018`。
