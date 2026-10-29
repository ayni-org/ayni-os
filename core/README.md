# Core 模块

Core 按不变量属主分为八个模块（ADR-0008、ADR-0042；原预算模块已取消）。每个模块的职责以计划 §2.3 为准，这里只记录实现层面的约定和当前状态，不重复 §2.3 的职责表。

## 依赖方向

```text
kernel → work → {authority, resources, knowledge} → catalog → durability → foundation
```

- 层次与子目录规则只在 [`tools/architecture.json`](../tools/architecture.json) 声明一次。CMake 的 `ayni_add_core_module` 据此在配置时拒绝向上或同层的链接依赖；`tools/check_deps.py` 据此检查 `#include`（ADR-0011）。
- 模块内的实体子目录（如 `work/task/`）之间默认不能互相包含；需要时在 `core_subdir_deps` 中逐对声明。模块根目录下的文件记作 `_root`，与子目录一样逐对声明，两个方向都要；每个模块声明的子目录图不得有环（ADR-0051）。嵌套路径按已声明边界的最长前缀匹配，runtime → service 的方向及服务内部约束同时检查（ADR-0058）。
- Core 内部头文件以模块名开头包含，例如 `#include "work/task/task.h"`。这个包含根只对 Core 模块自身可见，Core 之外只能使用 `include/ayni/` 中已实现的公开 API。
- Core 只能包含 `core_std_headers` 中列出的标准头文件，不包含平台、Host、runtime 或草案头文件。

## 当前状态

实际进度记在 [M1 交接状态](../reports/core/m1-progress.md)，Native E1～E4、SDK、覆盖与限制见 [Core Alpha 报告](../reports/core/m1-core-alpha.md)；各机制与验证见对应 ADR 和 [契约追踪](../contracts/traceability.md)。下表描述已有代码，不替代报告中的证据边界；ESP32 持久化仍未验收，每个改动模块职责的 PR 同时更新本表。

| 模块 | 内容 | 首次出现代码 |
|---|---|---|
| `kernel` | `api/`：C ABI 解码与具名生命周期闸门；`runtime/`：私有组合状态、facade、StepContext、生命周期/启动/提交/存储维护与 ingress；`service/`：model、intent、decision、call、execution、command 与持久实体编解码/重放（Agent/Goal/Task、Grant/command 等表使用各自 PersistentStore，ADR-0059、0064）；`config/`：启动目录与配置类型；`ports/`：共享完成槽、独立可靠租期控制邮箱、窄订阅 ingress 端口、锁和回调作用域守卫。依赖 api → runtime → service，service 内部仍按不变量分区；所有领域表继续归其原有模块（ADR-0058） | M0 |
| `work` | `agent/`：驻留实例的生命周期与记录；`goal/`：Goal 树、版本、终态让位、触发游标、失效、授权上限与记录；`task/`：Task 生命周期、订阅等待前沿、容量回收、步骤、私有检查点与授权快照；`subscription/`：Task 所有订阅的持久代际/租期控制事实；`call/`、`action/`：逻辑调用身份、保留的业务正文与副作用持久屏障；`execution/`：物理执行与停止事实；`authority/`：工作侧的准入证据与有界清理游标；`propagation`：跨表放行、派生选择与传播；`call_propagation`：调用的终态、对账与等待跳过如何落到 Task（ADR-0017～0027、ADR-0037、ADR-0038、ADR-0044、ADR-0053） | M1（`MA-004`） |
| `authority` | `principal/`：可信 Host 认证的所有者主体与会话表、持久 OwnerBinding 及记录 codec（ADR-0017、0028）；`scope/`：有限字段规范化、typed/encoded 覆盖与实际请求完整性（ADR-0029）；`grant/`：Grant 事实表与受限委派、独立 RevocationGate、GrantRestore 历史恢复、持久根撤销与有界清理（ADR-0030、0031、0059）；`scene/`：Scene 声明与持久所有者偏好（ADR-0050） | M1（`MA-004`） |
| `resources` | `admission/`：有界全组准入、Agent 轮转/实例 FIFO、等待期限/原因、健康/占用及带代际的执行器隔离；`permit/`：本次启动的许可占用、轮询与隔离（ADR-0046、ADR-0049、ADR-0050） | M1（`MA-018A`） |
| `knowledge` | `observation/`：本次启动的 Host/Task scoped 观测、有限租期、有效期与依赖身份，不做采集 I/O（ADR-0050、0063）；`memory/`：私有记忆事实、命名空间、正文与依赖；`context/`：受管理文件内容的发布状态与完整来源（ADR-0060） | M1（`MA-008`） |
| `catalog` | `catalog_id`：目录 ID 规则；`goal_type/`：GoalType 版本、触发、参数校验与 WorkProfile；`strategy/`：策略注册；`capability/`：能力目录、授权字段与调用契约；`resource/`、`device/`：资源与设备/组件描述；`provider/`、`binding/`：Provider 与绑定；`execution/`：执行端与执行绑定；`observation/`：观测来源（ADR-0029、ADR-0044～0050） | M1（`MA-004`） |
| `durability` | `batch/`：批次、记录 flags、记录类型登记、提交连续性及 writer fence 扫描；`journal/`：有界日志环；`snapshot/`：静态快照头部/尾标记及 CRC。只提供持久机制，不理解工作领域，不选择 Host，也不编排领域恢复次序（ADR-0016、0026、0027、0051） | M1（`MA-005`） |
| `foundation` | `crc32`：可跨页累计的 CRC-32/ISO-HDLC；`little_endian.h`：定长小端读写；`kernel_id.h`：带启动代际的 ID 与分配器；`transition_stamp.h`；`bounded_array.h`：调用方内存上的有界数组；`utf8`：Unicode scalar UTF-8 校验，公开纯函数 `ayni_validate_utf8` 与内部消费者共用同一规则（ADR-0082）。状态码仍只在公开头文件 `include/ayni/ayni.h` 定义 | M1（`MA-005`） |

模块在有源文件时才建立构建目标，避免空目标给人“已实现”的印象。`foundation` 承载 ID、时间、有界容器等内部类型；对外状态码仍以公开头文件为唯一来源，不在 Core 内复制一份。

## 内存

Core 不自行分配内存（ADR-0014）。调用方先用 `ayni_kernel_memory_requirements` 查询大小与对齐，再把内存块交给 `ayni_create`；Native 组合根从堆分配，ESP32 组合根从内部 RAM 分配。

日志与恢复、快照维护分时使用同一存储缓冲；维护先冻结新 transition 并排空 journal，再按完整记录分页，不复制 WorkModel，也不为整份快照分配另一块内存。默认缓冲为 `3 × max(W+4, 256)`，W 为按配置和各记录上界保守加总的 transition 批次预算；显式容量还须容纳最大的静态快照记录。预算包含 Grant 创建、启动到期结算、D 根撤销/清理 progress 及完整 auth mapping，不表示全部预留记录可在一条公开路径中同时发生。

调用方内存需求以 [`tools/measure_kernel_memory.c`](../tools/measure_kernel_memory.c) 的输出为准，本文不手写数字：它对默认值、最小工作容量、Native 双 Agent 调用和双 Agent 执行 4 个配置调用 `ayni_kernel_memory_requirements`，与 `runtime/common` 的默认配置一致（ADR-0053）。改变存储尺寸或放置的 PR 在说明中给出改前改后的输出；各版本的历史数字在对应 ADR（如 ADR-0027～0031、ADR-0051）。这里只是调用方内存，不包括 Host 的线程、文件缓存或运行时自身的分配，也不能替代 ESP32 RAM 与栈的实测。
