# Core 模块

Core 按不变量属主分为八个模块（ADR-0008、ADR-0042；原预算模块已取消）。每个模块的职责以计划 §2.3 为准，这里只记录实现层面的约定和当前状态，不重复 §2.3 的职责表。

## 依赖方向

```text
kernel → work → {authority, resources, knowledge} → catalog → durability → foundation
```

- 层次与子目录规则只在 [`tools/architecture.json`](../tools/architecture.json) 声明一次。CMake 的 `ayni_add_core_module` 据此在配置时拒绝向上或同层的链接依赖；`tools/check_deps.py` 据此检查 `#include`（ADR-0011）。
- 模块内的实体子目录（如 `work/task/`）之间默认不能互相包含；需要时在 `core_subdir_deps` 中逐对声明。模块根目录下的文件记作 `_root`，与子目录一样逐对声明，两个方向都要；每个模块声明的子目录图不得有环（ADR-0051）。
- Core 内部头文件以模块名开头包含，例如 `#include "work/task/task.h"`。这个包含根只对 Core 模块自身可见，Core 之外只能使用 `include/ayni/` 中已实现的公开 API。
- Core 只能包含 `core_std_headers` 中列出的标准头文件，不包含平台、Host、runtime 或草案头文件。

## 当前状态

`MA-010` 与 `MA-006` A–D 已合并，机制、限制与验证见 [ADR-0027](../docs/adr/0027-retries-writer-fences-snapshots-and-migration.md)、[ADR-0028](../docs/adr/0028-trusted-owner-binding-and-sessions.md)～[ADR-0031](../docs/adr/0031-revocation-gates-and-intent-receipts.md) 和 [契约追踪](../contracts/traceability.md)。整理及 E/F 的实际进度只记在 [M1 交接状态](../reports/core/m1-progress.md)。下表描述已有代码，不表示完整 E1–E4 或 ESP32 持久化验收已通过。

| 模块 | 内容 | 首次出现代码 |
|---|---|---|
| `kernel` | `kernel_api`、`work_api`：C ABI 参数检查、注册、会话、意图与查询；`kernel_lifecycle`：所有者线程的启动、工作、栅栏重载、维护与关闭编排；`kernel_layout`、`config/`：有界内存布局、配置与各存储的尺寸；`ingress/`、`completion/`：事件队列与完成槽；`boot/`：物理日志发现、快照与尾部恢复、writer 握手及启动记录；`commit/`、`command/`：日志提交与按 principal 有界的命令窗口；授权命令另保存原始 typed 规范请求，窗口与 scratch 均由 command 模块拥有，按完整字节比较；专用`pending_revoke`保存易失撤销身份/queued stamp，窗口统一pin防遗忘；`intent_receipt`编排三个阶段查询；`storage/`：共享写频与重试调度、日志上限与介质空间预算（`storage_budget`）、快照发布与轮转状态机（`storage_maintenance`）；`intent/`：工作模型、意图解码/规则与触发；`decision/`：策略决定；`call/`：调用运行时（准入、派发、完成、对账与执行控制）；`persistence/`：`work_journal`、`work_replay`、`work_snapshot` 按领域属主编排持久导出与恢复（ADR-0015～0027、ADR-0051） | M0 |
| `work` | `agent/`：驻留实例的生命周期与记录；`goal/`：Goal 树、版本、终态让位、触发游标、失效与记录；`task/`：Task 生命周期、容量回收、步骤与私有检查点；各表提供静态快照导入并保留原始 stamp，恢复的临时兼容覆盖与持久状态分开；`propagation`：跨表放行、派生选择与传播。调用与动作仍随后续包（ADR-0017～0027） | M1（`MA-004`） |
| `authority` | `principal/`：可信 Host 认证的所有者主体与会话表、持久 OwnerBinding 及记录 codec；失配恢复只读受限，缺授权元数据的恢复工作保留并禁行（ADR-0017、0028）；`scope/`：有限字段规范化、typed/encoded 覆盖与实际请求完整性（ADR-0029）；`grant/`：Grant 创建/受限委派、typed 查询、当前 boot 期限、持久终态叶节点回收与记录 codec，保存完整源域和展开 Scope（ADR-0030）；D新增独立易失Gate、全祖先拒绝、持久根撤销/有界清理cursor与独立cleanup_stamp（ADR-0031，#28 已合并）；审批/完整工作准入随 E/F，Scene 授权仍不支持 | M1（`MA-004`） |
| `resources` | `admission/`：有界全组准入、Agent 轮转/实例 FIFO、等待期限/原因、健康/占用及带代际的执行器隔离；尚未接 Provider 消费链 | M1（`MA-018A`，当前分支） |
| `knowledge` | 无代码 | M1（`MA-011`） |
| `catalog` | `catalog_id`：目录 ID 规则；`goal_type/`：GoalType 版本、触发种类及参数校验；`strategy/`：策略版本、决定和检查点上限注册。`capability/`：有界能力 ID/版本目录及授权字段类型、单位、用途、取值域和规范 codec（ADR-0029）；`resource/` 保存可信资源描述；设备、调用属性与绑定随 `MA-008` | M1（`MA-004`） |
| `durability` | `batch/`：schema 0/1 批次、记录 flags、提交连续性及 writer fence 扫描；`journal/`：有界日志环；`snapshot/`：静态快照头部/尾标记及 CRC。只提供持久机制，不理解工作领域，不选择 Host，也不编排领域恢复次序（ADR-0016、0026、0027） | M1（`MA-005`） |
| `foundation` | `crc32`：可跨页累计的 CRC-32/ISO-HDLC；`little_endian.h`：定长小端读写；`kernel_id.h`：带启动代际的 ID 与分配器；`transition_stamp.h`；`bounded_array.h`：调用方内存上的有界数组。状态码仍只在公开头文件 `include/ayni/ayni.h` 定义 | M1（`MA-005`） |

模块在有源文件时才建立构建目标，避免空目标给人“已实现”的印象。`foundation` 承载 ID、时间、有界容器等内部类型；对外状态码仍以公开头文件为唯一来源，不在 Core 内复制一份。

## 内存

Core 不自行分配内存（ADR-0014）。调用方先用 `ayni_kernel_memory_requirements` 查询大小与对齐，再把内存块交给 `ayni_create`；Native 组合根从堆分配，ESP32 组合根从内部 RAM 分配。

日志与恢复、快照维护分时使用同一存储缓冲；维护先冻结新 transition 并排空 journal，再按完整记录分页，不复制 WorkModel，也不为整份快照分配另一块内存。默认缓冲为 `3 × max(W+4, 256)`，W 为按配置和各记录上界保守加总的 transition 批次预算；显式容量还须容纳最大的静态快照记录。预算包含 Grant 创建、启动到期结算、D 根撤销/清理 progress 及完整 auth mapping，不表示全部预留记录可在一条公开路径中同时发生。

ABI 0.15 在本次 64 位环境中经 `ayni_kernel_memory_requirements` 实测：

| 配置 | 字节数 | 较 ABI 0.14 | 对齐 |
|---|---:|---:|---:|
| 默认值（配置 `struct_size=12`） | 62,600 | +4,656 | 8 |
| 最小工作容量（完整配置，命令容量 1） | 28,808 | +1,464 | 8 |

#23 整理代码时把维护与预算状态收进 `StorageMaintenance`、`StorageBudget` 并重排字段，少了 8 字节对齐填充，两组数字都比 #21 的实测（ADR-0027）少 8 字节。

这里是调用方内存需求，不包括 Host 的线程、文件缓存或运行时自身的分配；复现配置及完整验证结果见 ADR-0027。

ABI 0.16（ADR-0028，MA-006 A）在相同环境实测默认配置 62,712 B、最小工作容量 28,920 B，均比上表 0.15 增加 112 B，对齐仍为 8 B。新增绑定保存在现有 authority 聚合内；日志批次使用包含首次绑定记录的保守预算，不据此宣称该组合可在一次运行中同时发生或 ESP32 RAM 门槛已验收。

ABI 0.17（ADR-0029，MA-006 B）在相同环境实测默认配置 **67,544 B**、最小工作容量 **29,060 B**，比 A 的 0.16 分别增加 **4,832 B / 140 B**，对齐仍为 8 B。Capability 目录由 WorkModel 持有，metadata 与固定字节 stride 通过同一 `CapabilityRegistry::storage_size` 接入 arena；默认新增 16 条、每条 256 B 域正文，最小配置取 1 条、64 B。配置、查询程序与原始记录见 ADR-0029；Native 的增量不能替代 ESP32 RAM 验收。

ABI 0.18（ADR-0030，MA-006 C）同环境实测默认配置 **106,520 B**、最小工作容量 **31,072 B**，比 B 分别增加 **38,976 B / 2,012 B**，对齐仍为 **8 B**。GrantTable 拥有每 Grant 的 metadata、完整 source domain/展开 scope 两个字节 stride；CommandTable 在同一 arena 中拥有每个 mapping 的 canonical stride 和一个 scratch，journal 预留由各记录上界统一计算。原始查询程序与日志在 `../ayni-ma006-evidence/c/memory.c`、`memory.log`；独立 coverage 产物以严格 C99 编译同一 probe，结果一致（`coverage/memory-independent.log`），其 profile 输出与覆盖率采集隔离。这些数字只证明 Native 调用方内存需求，不是 ESP32 RAM/栈或硬件验收。

ABI 0.19（ADR-0031，MA-006 D）同环境多轮严格C99量测：默认 **110,792 B**、最小工作配置 **31,616 B**，较C分别 **+4,272 B / 544 B**，对齐仍为 **8 B**（`../ayni-ma006-evidence/d/coverage/round1/`、`coverage/final2/memory.log`）。栈/最坏step成本与ESP32资源仍待实测。GrantTable拥有metadata与独立Gate arena；CommandTable拥有pending metadata（容量=grant_capacity）、原16 B canonical、唯一结果窗口及scratch，不把新状态散加至Kernel。自动reload的clear_restored保留Gate/pending而清领域事实及旧queued stamp，完整clear才释放全部生命周期；Gate/pending/pin均不持久。Grant snapshot尾增至80 B，旧40 B版本继续读取，journal与snapshot上下文分类明确区分。结构/公式只能说明有界分配，不能替代量测或ESP32验收。
