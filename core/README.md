# Core 模块

Core 按不变量属主分为九个模块（ADR-0008）。每个模块的职责以计划 §2.3 为准，这里只记录实现层面的约定和当前状态，不重复 §2.3 的职责表。

## 依赖方向

```text
kernel → work → {authority, resources, budget, knowledge} → catalog → durability → foundation
```

- 层次与子目录规则只在 [`tools/architecture.json`](../tools/architecture.json) 声明一次。CMake 的 `ayni_add_core_module` 据此在配置时拒绝向上或同层的链接依赖；`tools/check_deps.py` 据此检查 `#include`（ADR-0011）。
- 模块内的实体子目录（如 `work/task/`）之间默认不能互相包含；需要时在 `core_subdir_deps` 中逐对声明。模块根目录下的文件可以包含本模块的任何子目录。
- Core 内部头文件以模块名开头包含，例如 `#include "work/task/task.h"`。这个包含根只对 Core 模块自身可见，Core 之外只能使用 `include/ayni/` 中已实现的公开 API。
- Core 只能包含 `core_std_headers` 中列出的标准头文件，不包含平台、Host、runtime 或草案头文件。

## 当前状态

`MA-010` E+F、`MA-006` A/B 的实现与验证范围见 [ADR-0027](../docs/adr/0027-retries-writer-fences-snapshots-and-migration.md)、[ADR-0028](../docs/adr/0028-trusted-owner-binding-and-sessions.md)、[ADR-0029](../docs/adr/0029-capability-domains-and-scope-algebra.md) 与 [契约追踪](../contracts/traceability.md)。下表描述已有代码，不表示完整 E1–E4 或 ESP32 持久化验收已通过。

| 模块 | 内容 | 首次出现代码 |
|---|---|---|
| `kernel` | `kernel_api`、`work_api`：C ABI 参数检查、注册、会话、意图与查询；`kernel_lifecycle`：所有者线程的启动、工作、栅栏重载、维护与关闭编排；`kernel_layout`、`kernel_config`：有界内存与配置；`ingress/`、`completion/`：事件队列与完成槽；`boot/`：物理日志发现、快照与尾部恢复、writer 握手及启动记录；`commit/`、`command/`：日志提交与按 principal 有界的命令窗口；`storage/`：共享写频与重试调度、日志上限与介质空间预算（`storage_budget`）、快照发布与轮转状态机（`storage_maintenance`）；`intent/`：工作模型、意图解码/规则、触发和策略决定；`work_journal`、`work_replay`、`work_snapshot`：按领域属主编排持久导出与恢复（ADR-0015～0027） | M0 |
| `work` | `agent/`：驻留实例的生命周期与记录；`goal/`：Goal 树、版本、终态让位、触发游标、失效与记录；`task/`：Task 生命周期、容量回收、步骤与私有检查点；各表提供静态快照导入并保留原始 stamp，恢复的临时兼容覆盖与持久状态分开；`propagation`：跨表放行、派生选择与传播。调用与动作仍随后续包（ADR-0017～0027） | M1（`MA-004`） |
| `authority` | `principal/`：可信 Host 认证的所有者主体与会话表、持久 OwnerBinding 及记录 codec；失配恢复只读受限，缺授权元数据的恢复工作保留并禁行（ADR-0017、0028）；`scope/`：有限字段规范化、typed/encoded 覆盖与实际请求完整性（ADR-0029）；Grant 与审批随后续 `MA-006` 部分，Scene 授权随 `MA-026` | M1（`MA-004`） |
| `resources` | 无代码 | M1（`MA-018`） |
| `budget` | 无代码 | M1（`MA-018`） |
| `knowledge` | 无代码 | M1（`MA-011`） |
| `catalog` | `catalog_id`：目录 ID 规则；`goal_type/`：GoalType 版本、触发种类及参数校验；`strategy/`：策略版本、决定和检查点上限注册。`capability/`：有界能力 ID/版本目录及授权字段类型、单位、用途、取值域和规范 codec（ADR-0029）；设备、调用属性与绑定随 `MA-008` | M1（`MA-004`） |
| `durability` | `batch/`：schema 0/1 批次、记录 flags、提交连续性及 writer fence 扫描；`journal/`：有界日志环；`snapshot/`：静态快照头部/尾标记及 CRC。只提供持久机制，不理解工作领域，不选择 Host，也不编排领域恢复次序（ADR-0016、0026、0027） | M1（`MA-005`） |
| `foundation` | `crc32`：可跨页累计的 CRC-32/ISO-HDLC；`little_endian.h`：定长小端读写；`kernel_id.h`：带启动代际的 ID 与分配器；`transition_stamp.h`；`bounded_array.h`：调用方内存上的有界数组。状态码仍只在公开头文件 `include/ayni/ayni.h` 定义 | M1（`MA-005`） |

模块在有源文件时才建立构建目标，避免空目标给人“已实现”的印象。`foundation` 承载 ID、时间、有界容器等内部类型；对外状态码仍以公开头文件为唯一来源，不在 Core 内复制一份。

## 内存

Core 不自行分配内存（ADR-0014）。调用方先用 `ayni_kernel_memory_requirements` 查询大小与对齐，再把内存块交给 `ayni_create`；Native 组合根从堆分配，ESP32 组合根从内部 RAM 分配。

日志与恢复、快照维护分时使用同一存储缓冲；维护先冻结新 transition 并排空 journal，再按完整记录分页，不复制 WorkModel，也不为整份快照分配另一块内存。默认缓冲为 `3 × max(W+4, 256)`，W 为配置允许的最大 transition 批次；显式容量还须容纳最大的静态快照记录。

ABI 0.15 在本次 64 位环境中经 `ayni_kernel_memory_requirements` 实测：

| 配置 | 字节数 | 较 ABI 0.14 | 对齐 |
|---|---:|---:|---:|
| 默认值（配置 `struct_size=12`） | 62,600 | +4,656 | 8 |
| 最小工作容量（完整配置，命令容量 1） | 28,808 | +1,464 | 8 |

#23 整理代码时把维护与预算状态收进 `StorageMaintenance`、`StorageBudget` 并重排字段，少了 8 字节对齐填充，两组数字都比 #21 的实测（ADR-0027）少 8 字节。

这里是调用方内存需求，不包括 Host 的线程、文件缓存或运行时自身的分配；复现配置及完整验证结果见 ADR-0027。

ABI 0.16（ADR-0028，MA-006 A）在相同环境实测默认配置 62,712 B、最小工作容量 28,920 B，均比上表 0.15 增加 112 B，对齐仍为 8 B。新增绑定保存在现有 authority 聚合内；日志批次使用包含首次绑定记录的保守预算，不据此宣称该组合可在一次运行中同时发生或 ESP32 RAM 门槛已验收。

ABI 0.17（ADR-0029，MA-006 B）在相同环境实测默认配置 **67,544 B**、最小工作容量 **29,060 B**，比 A 的 0.16 分别增加 **4,832 B / 140 B**，对齐仍为 8 B。Capability 目录由 WorkModel 持有，metadata 与固定字节 stride 通过同一 `CapabilityRegistry::storage_size` 接入 arena；默认新增 16 条、每条 256 B 域正文，最小配置取 1 条、64 B。配置、查询程序与原始记录见 ADR-0029；Native 的增量不能替代 ESP32 RAM 验收。
