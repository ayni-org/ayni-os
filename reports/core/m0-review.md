# M0 复核报告

- **范围**：计划 §9 M0 Exit Gate；`MA-017` 完成定义（契约覆盖与 ADR 一致性）；`MA-002` 工程骨架；`MA-001` 板级可行性的状态；M1 估算复核
- **日期**：2026-10-19
- **代码**：分支 `claude/project-thread-6xckzw`（PR #6）
- **结果等级**：文档复核，加上本地与 CI 的编译和测试结果。没有任何实机结果。

## 结论

M0 中不依赖开发板的部分已经完成；依赖开发板与询价的两项还没有完成，因此 M0 Exit Gate **尚未全部通过**。

| Exit Gate 项（计划 §9） | 状态 | 证据 |
|---|---|---|
| 分层、函数、提交与输出门控、执行许可与失效、信任模型、Fabric 身份/新鲜度/幂等、五项语义、版本/API | 完成 | `contracts/` 共 295 条条款；C 头文件草案在 `include/ayni/draft/`；本报告第 1 节 |
| E1–E4 规范 | 完成（格式与用例清单） | [`contracts/fixtures/`](../../contracts/fixtures/)：格式、动作与断言表，E1–E4 用例清单；可运行用例在 M1 |
| 可选 R1 规范及 ROS/MHS 边界 | 边界完成；R1 规范按用户决定推迟（D4，`MA-034` 不在 M0 做） | C-TRUST-003、C-TRUST-005、C-WIRE-015 |
| 空 Core / ESP Hello Host 可启动 | Native 完成；ESP32 固件已编译，未在实机启动 | 第 3 节；[board-feasibility.md](../hardware/m0/board-feasibility.md) |
| 含停止链硬件的 ≤300 元方案与板级可行性有证据 | 未完成：候选电路与引脚表已完成，模块价格未询价，板型未确认 | [`docs/hardware/stop-chain.md`](../../docs/hardware/stop-chain.md)、[bom-candidates.md](../hardware/m0/bom-candidates.md)、[gpio-candidates.md](../hardware/m0/gpio-candidates.md) |
| 可选：具身 Profile 草案 | 不做（D4） | — |

剩余两项只影响 ESP32 实机与 M3 硬件集成，不影响 M1 的 Native Core 开发。是否在这两项关闭之前开始 M1，需要用户决定（第 6 节）。

## 1. 契约覆盖（`MA-017`）

### 方法

`MA-017` 要求第 2.5/2.7/2.8/2.10/3.4/3.6 节及五项产品语义“可判定”。复核逐节阅读计划正文，把每一条规范性陈述对应到条款；没有对应条款的，判断它是缺漏、范围说明还是可选扩展，并在本节末尾列出处理。机械部分由 `tools/check_contracts.py` 完成（ADR-0013）：每条条款四列齐全、反例与验证非空、ID 唯一且位于前缀所属文档、全仓库引用的 ID 都有定义、验证列点名的测试真实存在、E1–E4 的每个步骤至少被一条条款点名。

### 第 2.5 节 策略与信任边界

| 计划规则 | 条款 |
|---|---|
| Kernel 校验全部意图；策略不能改内部对象、绕过资源入口、提权或自行恢复未知动作 | C-POLICY-003 |
| 回调有工作量限制，复杂计算放异步 Provider | C-POLICY-002、C-POLICY-011 |
| 有界决策函数：`step` 内以只读快照与私有检查点调用，返回有界意图与新检查点；不调用 API、不做 I/O、不阻塞 | C-POLICY-001、C-POLICY-002、C-POLICY-004 |
| Tool Loop 的模型调用以模型请求意图表达，结果出现在后续快照 | C-POLICY-005 |
| 形态服务于确定性重放与策略替换 | C-POLICY-006、C-POLICY-010 |
| 模型、VLA、外部消息、发现的描述、MHS 文件是不可信数据，经 Kernel 校验 | C-TRUST-001、C-TRUST-002 |
| 适配器独立进程、独立账户、不访问执行器节点与凭据、经带主体身份的本地 IPC | C-TRUST-003 |
| 执行端拒绝缺少有效许可的指令 | C-TRUST-004、C-PERMIT-007 |
| 第三方原生代码与 MHS 代码不进入固件，只接受可转换为有界 Procedure 数据的子集 | C-TRUST-005 |
| 纯函数接口不是安全边界；进程内原生代码可信；ESP32 首版不提供强进程隔离 | 范围说明，见发现 F7 |
| ROS 侧 SROS2 严格模式，许可门独占控制器命令接口 | 未成条款，见发现 F6 |
| 未来第三方扩展声明平台隔离等级 | v0.1 范围外（计划 §9 M6“隔离独立验收”） |

### 第 2.7 节 函数层级、能力绑定与最小组合

| 计划规则 | 条款 |
|---|---|
| Scene 不兼任执行器，协调器不直接修改 Kernel 对象 | C-PRIM-002；结构检查见发现 F8 |
| Strategy 不私自 I/O、不持驱动锁，模型输出不是授权 | C-POLICY-001、C-RES-001、C-TRUST-001 |
| 统一入口不接受任意字符串代码或无 Schema 的字典 | C-CALL-001 |
| 语义调用身份是 `step_key`/`logical_call_id`，不是一次传输或函数指针 | C-ID-003、C-CALL-002 |
| Provider/Host/Driver 不写领域对象；底层控制细节不逐次成为业务步骤 | C-PROV-008、C-PERMIT-014 |
| 注册时核对类型、Schema/语义版本、调用形态、模态、资源与取消/查询特性 | C-BIND-001 |
| 同名能力可有多个绑定，只在满足权限、外发与预算的候选中准入 | C-BIND-002 |
| 准备记录保存绑定与版本；超出批准费用或目的地的 fallback 须重新授权 | C-BIND-003、C-BUDGET-005 |
| 越过屏障后禁止静默切换实现；换绑定须先取得权威未执行结论并封禁旧 ID | C-CALL-008、C-ID-004 |
| 能力分别声明副作用类别、幂等性、去重窗口、查询权威性、取消方式与补偿 | C-CALL-001（属性表在 `calls_and_bindings.md`） |
| `NOT_FOUND` 不证明未执行；权威未执行的成立条件 | C-CALL-010、C-PROV-005、C-WIRE-008 |
| 纯计算不需要动作账本；计费模型请求与媒体上传持久绑定调用、预算与外发证据 | C-CALL-005 |
| 稳定 `step_key`；`logical_call_id` 与 `operation_id` 首次接受时保存，重启与换策略不重新生成 | C-ID-003、C-POLICY-008 |
| 共同检查点的内容；私有检查点只存算法状态；不兼容时暂停 | C-CALL-004、C-LIFE-024 |
| 资源按当前步骤申请；组合不持有父级资源等待子调用 | C-CALL-006、C-RES-002 |
| `blocked_on` 是派生视图 | C-PRIM-003 |
| 单轮最多 8 个调用 | C-CALL-006、C-LIMIT-006 |
| 未知步骤阻止依赖步骤；已完成不重做；未派发重新准入；补偿是新调用 | C-CALL-006、C-CALL-007 |

### 第 2.8 节 逻辑提交与一致性不变量

| 计划规则 | 条款 |
|---|---|
| `append` 以 `OK/FAILED/UNKNOWN` 完成，对外只暴露 `durable_seq` | C-COMMIT-001、C-HOST-009 |
| 批次有长度、校验与提交标记；恢复返回持久前缀并忽略不完整尾部；重放不重复应用 | C-COMMIT-002、C-STORE-001、C-STORE-002 |
| Kernel 只有一份内存状态，立即应用，不建暂存副本，不在内存中回滚部分 transition | C-COMMIT-003 |
| 输出带 `requires_seq`；查询标注 `durable_seq` | C-COMMIT-004 |
| 副作用 submit 前最终复核，复核与 submit 之间不处理其他普通意图 | C-COMMIT-007 |
| 写失败或未知时以同一 transition 有界重试，仍失败进入受限栅栏 | C-COMMIT-008、C-COMMIT-009 |
| 重载前排空旧写入方或安装新 writer epoch，并写栅栏记录 | C-COMMIT-010、C-HOST-012、C-STORE-005 |
| 两者都无法确认时只读诊断，本地停止路径照常 | C-COMMIT-011 |
| 以新 `boot_epoch` 重载；在途动作转 `RESULT_UNKNOWN`；易失缓冲不直接改写状态 | C-ID-009、C-COMMIT-009、C-OUT-002 |
| ESP32 上从写失败到重载完成的耗时须实测 | 测量要求，见发现 F9 |
| 五组最小提交批次 B1–B5 | C-COMMIT-005、C-WIRE-004、C-BUDGET-002、C-PROD-030 |
| 跨存储介质须有可验证的协调日志 | C-COMMIT-014 |
| 外部副作用不参加本地原子事务 | C-COMMIT-013、C-COMMIT-006 |
| 存储提交响应未知时按原 transition 恢复，禁止新建身份重做 | C-COMMIT-008 |
| 策略意图携带 `expected_revision`，过时写入返回冲突 | C-COMMIT-012 |

### 第 2.10 节 执行许可、失效与停止上限

| 计划规则 | 条款 |
|---|---|
| 每个控制域唯一 authority-owner；执行端逐周期校验；执行点只能收紧 | C-PERMIT-001、C-PERMIT-003、C-AUTH-016 |
| 过程与会话两种形态在各自粒度使用屏障与 ActionRecord；包络内样本不成为 operation | C-PERMIT-002、C-PERMIT-014 |
| 扩大包络、改变控制模式或权限须重新准入 | C-PERMIT-003 |
| 许可字段：身份与范围、五种代际、租期 | C-PERMIT-004、C-PERMIT-010 |
| 回显 nonce；截止 `max(截止, t_N + L)`；nonce 轮换，未知或淘汰的拒绝 | C-PERMIT-006 |
| 越过截止即终态，须以新 `permit_id` 重新准入，停止确认前保持隔离 | C-PERMIT-008、C-RES-008 |
| 在途续租不能把截止推过撤销时刻 + `L`；`L` 含时钟漂移余量 | C-PERMIT-005、C-PERMIT-010 |
| 每个控制周期检查代际、租期、停止锁存、校准与指令 | C-PERMIT-007 |
| 失效表（撤销、传播失败、Kernel 失效与受限栅栏、执行端重启、校准失效、急停、急停复位、换主/分区） | C-PERMIT-001、C-PERMIT-004、C-PERMIT-005、C-PERMIT-009、C-PERMIT-012、C-PERMIT-013、C-STOP-002、C-STOP-003 |
| 会话租期按风险与实测停止时间设定；过程最长执行时长构成停止上限；无法给出停止上限的能力不开放会话 | C-PERMIT-010、C-PERMIT-011 |
| 静态准入由 Kernel；持续联锁由执行端声明与执行；独立停止覆盖所有控制者 | C-AUTH-016、**C-PERMIT-016（本次新增，F1）** |
| 许可结束不等于资源可分配；Provider 不能无限续租 | C-RES-008 |
| 运动结果四个阶段及证据；无反馈的限时动作只报告时长与估计 | C-STOP-007 |
| 成功条件由 Capability/GoalType 声明，命令完成或电机停止不推出目标达成 | C-POLICY-009、C-STOP-007 |
| 物理结果未知时不盲目重复 | C-STOP-010 |
| 跨设备时全部校验在执行端；以 nonce 与本地租期证明新鲜度 | C-PERMIT-015 |

### 第 3.4 节 管理 API 与统一消息信封

| 计划规则 | 条款 |
|---|---|
| 局域网管理传输经认证；停止入口连接独立本地停止路径 | C-WIRE-002 |
| 事件订阅带游标，超出保留范围明确返回 gap | C-WIRE-013 |
| 每种消息只有一个幂等键 | C-WIRE-003、C-ID-008 |
| `command_id` 的作用域、参数摘要、重试返回原对象、冲突返回 | C-WIRE-004、C-WIRE-012 |
| 幂等映射与业务接受结果同批提交 | C-WIRE-004、C-COMMIT-005 |
| 会话重连不改变主体作用域 | **C-WIRE-016（本次新增，F3）** |
| 保留期、主体命令代际与拒绝标记；不合并参数相同的两个目标 | C-WIRE-005 |
| 内部周期触发以 occurrence 去重 | C-TIME-005、C-LIMIT-007 |
| 控制消息为有限长 JSON；媒体走独立通道与受控引用 | C-JSON-001、C-WIRE-011、C-PRIM-005 |
| 运动配置名须由目标设备匹配到实测、已启用的参数上限，本身不是校准证据或授权 | **C-PERMIT-017（本次新增，F2）** |
| `principal_ref`、`grant_ref`、`binding_id` 只是引用，不信任自报主体 | C-AUTH-012 |
| 节点认证只证明节点；实例映射注册表；敏感命令另需管理端证据 | C-AUTH-013、C-AUTH-009 |
| Grant 由接收方保存，不作持有者令牌 | C-AUTH-014 |
| 查询、取消与订阅鉴权 | C-AUTH-015 |
| `message_id` 不承担业务幂等 | C-ID-007 |
| `step_key` 与 `logical_call_id` | C-ID-003、C-POLICY-008 |
| `operation_id` 的作用域、参数摘要与映射保存时机 | C-WIRE-006、C-ID-004 |
| `operation_id` 单调序号、低水位、`EXPIRED`、保留期 | C-ID-005（本次补充保留期，F4） |
| `request_id` 与 `boot_epoch` | C-ID-006 |
| Goal/Task 固定 owner，远程调用不转移归属 | C-LIFE-020 |
| 接收设备重新校验主体与授权，相同 Agent ID 不免检 | C-AUTH-002、C-AUTH-005、C-PRIM-007 |
| TTL 语义、`receiver_tick` 与 `STALE` | C-WIRE-009 |
| 查询结果 `FOUND`、`EXPIRED_UNKNOWN`、`NOT_ACCEPTED` | C-WIRE-008 |
| 控制帧 8 KiB、嵌套深度 8，数值、UTF-8、未知必需字段校验 | C-JSON-001、C-JSON-002、C-JSON-003、C-JSON-005、C-JSON-006；锁定时点见发现 F5 |
| 不在控制 JSON 中放 base64 大文件 | C-WIRE-011 |
| 五种接收结果；不宣称通用的“恰好执行一次” | C-WIRE-007 |

### 第 3.6 节 调用形态、订阅与领域类型

| 计划规则 | 条款 |
|---|---|
| `unary`、`action`、`subscription` 的最小生命周期 | `calls_and_bindings.md` 调用形态表；C-CALL-003、C-LIFE-011 |
| 可取消不等于未计费或未外发 | C-PROV-006 |
| 订阅在 `ACTIVE` 前不接纳观察；续租重查权限与预算；停止后尾部事件不用；重启用新代际 | C-SUB-001、C-SUB-002、C-SUB-003 |
| 事件声明可合并、可丢弃与 gap；遥测不承诺无限历史 | C-SUB-004 |
| 订阅代际与 `boot_epoch` 一同校验 | C-SUB-002 |
| 模型流式输出单独协商 | C-PROV-010 |
| 有限类型集合与 JSON Schema 子集 | C-JSON-003、C-JSON-008、C-JSON-011、C-JSON-012 |
| 64 位值用字符串；禁止 NaN/Infinity；缺省、`null`、未知字段含义固定 | C-JSON-004、C-JSON-005、C-JSON-006 |
| Schema 合法与语义合法分开检查；物理参数做语义校验 | C-JSON-009、C-JSON-005、C-PROV-003 |
| ObjectRef 最小字段与校验 | C-WIRE-010 |
| 远程证据不替代本地证明的物理前提 | C-PRIM-004 |
| 不比较不同设备的单调时间 | C-TIME-002、C-JSON-010 |
| MCP/A2A 适配的映射；外部注解不成为可信属性；不伪造去重或查询；不支持时显式返回 | C-WIRE-015 |

### 五项产品语义（计划 §14.1）

| 产品语义 | 条款 |
|---|---|
| 助理公共档案 | C-PROD-040～042 |
| 所有者与语音 | C-PROD-001～004、C-AUTH-009 |
| 暂停助理 | C-PROD-020～023、C-LIFE-023、C-STOP-002 |
| 忘记 | C-PROD-030～035 |
| 提醒完成 | C-PROD-010～017 |

### 发现与处理

| 编号 | 发现 | 处理 |
|---|---|---|
| F1 | §2.10 的“持续联锁由执行端声明与执行、独立停止覆盖所有控制者”没有条款 | 新增 C-PERMIT-016 |
| F2 | §3.4 中运动配置名（如 `calibrated-short-turn`）的规则没有条款 | 新增 C-PERMIT-017 |
| F3 | §3.4 的“会话重连不改变主体作用域”没有条款 | 新增 C-WIRE-016，并把 E2 F9 用例改为在新会话中重试 |
| F4 | `operation_id` 去重记录的保留期没有写进条款 | 补入 C-ID-005 |
| F5 | §3.4 写“消息限额 M0 测试后锁定”，但 M0 没有 JSON 解码器，无法测试 | 计划 v1.6 改为“JSON 解码器实现时（`MA-038` 或 `MA-007` 中较早者）测试后锁定”；在此之前按计划数值执行 |
| F6 | §2.5 中 ROS 侧 SROS2 严格模式与许可门的要求没有条款 | 属于可选 Linux ROS 2 Bridge（`MA-036`、R1.b）；实现该扩展时补条款，不进入 v0.1 必需契约 |
| F7 | “纯函数接口不是安全边界”“进程内原生代码可信”“ESP32 首版无强进程隔离”是不作保证的范围说明 | 写在 `policy_interface.md` 与 `authority_and_trust.md` 正文；没有可测试的行为，不写成条款 |
| F8 | “协调器不直接修改 Kernel 对象”是结构约束 | `tools/check_deps.py` 禁止 `core/` 以外包含 Core 内部头文件，且未登记的顶层源码目录直接报错；建立 `applications/`、`strategies/`、`providers/` 时须在 `tools/architecture.json` 登记其规则 |
| F9 | “ESP32 上从写失败到重载完成的耗时须实测”是测量要求 | 在 M2 `MA-024` 的资源测量中记录 |
| F10 | 计划 §3.2 的接口节选已落后于实现：`ayni_create` 由调用方提供内存（ADR-0014），`ayni_destroy` 返回状态码，`ayni_step` 增加了报告输出 | 计划 v1.6 用指向 `include/ayni/` 的说明代替节选，避免两处维护同一接口 |
| F11 | 计划 §9.1 的 `MA-002` 完成定义仍写“macOS ARM64 空 Core”；§6、§10.6、§11.1 同样写 macOS 为当前开发机 | 按用户决定（Linux 优先）在 v1.6 修正 |
| F12 | 预算表述仍是“不超过 300 元、不可私自加预算” | 按用户决定（300 元为目标，可视实际增加）在 v1.6 与两份 README 修正；增加的部分仍须列明原因与替代方案 |

处理之后，上述各节的规范性规则都有对应条款或明确的去向；`check_contracts_repository` 通过，追踪矩阵已重新生成。

## 2. ADR 与正文一致性

- ADR-0001～0008、0010 的决策都已写入契约，追踪矩阵的“ADR → 契约文档”表列出了对应文档（ADR-0010 规定组合根与目标目录，属于构建结构，由 `check_deps` 与 CMake 层级检查覆盖，不对应契约条款）。复核没有发现契约与这些 ADR 的语义冲突；唯一的不一致是 F10 所述的计划 §3.2 节选。
- ADR-0009（IDL 与本地 API 编码）保持“提议”，按计划在 M1 早期实测后决定；ADR-0013 规定了在此之前的契约表达方式。
- 本 PR 新增 ADR-0011～0014，`docs/adr/README.md` 已登记，计划 v1.6 的 ADR 清单同步。

## 3. 工程骨架（`MA-002`）与 ADR-0011 的证据

| 项 | 结果 |
|---|---|
| 工具链锁定 | 版本与命令见 `docs/development.md`；GoogleTest 1.17.0 按 SHA-256 锁定；ESP-IDF v6.0.3 |
| 本地测试 | `ctest --test-dir build/native`：36 个用例全部通过（GCC Debug，2026-10-19）；此前在 GCC ASan/UBSan、GCC Release 与 Clang Debug 下同样通过 |
| 依赖规则 | `check_deps_repository` 通过；`layers_reject_*` 与 `layers_accept_lower_layer_dependency` 证明 CMake 层级检查能拒绝向上、同层与未知依赖 |
| Core 不分配内存（ADR-0014） | `core_does_not_reference_heap_allocator` 通过；`heap_check_detects_allocator` 证明该检查确实能发现分配器引用 |
| 头文件 | 每个公开与草案头文件单独、以及合在一起，都以 C99 `-pedantic-errors` 与 C++17 编译 |
| CI | 依赖/契约/格式检查、Native 矩阵（GCC、Clang、ASan/UBSan）、clang-tidy（仅报告）与 ESP32-S3 固件构建；PR #6 在 `41f55f4` 上 7 项检查全部通过 |
| ESP32 Hello | 本地与 CI 编译通过，镜像 174,047 字节；未烧录 |
| 偏差 | 计划要求“macOS ARM64 空 Core”；用户决定以 Linux 为开发机，macOS 未验证（F11） |

## 4. 板级可行性（`MA-001`）

状态与到板后的步骤见 [board-feasibility.md](../hardware/m0/board-feasibility.md)。引脚分配、停止链候选电路与候选 BOM 均为资料推演；实测启动日志、板型差异与预算合计未完成。

## 5. M1 估算复核

计划要求 M0 结束时“按分包实测工作量复核”估算。M0 由 Claude 在一次会话中完成，耗时不能代表单人全职的工作量，所以**没有可用于重新标定的工作量数据**。本节只复核范围：

- 295 条条款中，226 条的验证列指向 E1–E4 或 M1 工作包，是 M1 需要实现并测试的范围；按文档分布，最多的是资源与预算（22）、五项产品语义（21）、调用与绑定（19）、提交与输出（18）、执行许可与停止（18）。
- 原估算之外新增、但已经确定要做的工作：E1–E4 的用例生成器与两个运行器（ADR-0012、`contracts/fixtures/`），ADR-0009 的 IDL 选型实测，`ayni_config_v1` 的容量字段与内存需求计算（ADR-0014）。
- 建议保持 M1 5–7 周、`MA-038` 另加 1–2 周的估算不变，把用例基础设施计入 `MA-003` 与 `MA-005`；完成 `MA-003`、`MA-005`、`MA-010` 后用实际记录复核一次。从 M1 起每个工作包在报告中记录开始与结束日期，下次复核才有数据。

## 6. 需要用户决定或提供的事项

| 事项 | 选项与影响 |
|---|---|
| 是否在 M0 板级两项关闭前开始 M1 | 建议开始：M1 是 Native Core，不依赖开发板；板级两项须在 M2（ESP32 复核）前关闭。不开始则等板子与询价 |
| 开发板 | 到手后按 board-feasibility.md 自检，提供照片与模组丝印 |
| 模块询价 | BOM 中“待询价”各项，用于判断 300 元目标是否可行 |
| 许可证（D6） | 仍未决定；仓库私有期间不影响开发 |
| 急停切断位置、录音键与 GPIO0、电源方案 A/B | 已在硬件文档中给出推荐，可在 M3 前确认 |

## 7. 执行过的检查

```sh
python3 tools/check_contracts.py            # 通过，295 条条款
python3 tools/check_deps.py                 # 通过
ctest --test-dir build/native               # 36/36 通过
idf.py size                                 # targets/esp32_assistant，结果见 board-feasibility.md
git diff --check
```
