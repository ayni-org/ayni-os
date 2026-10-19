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

| 模块 | 内容 | 首次出现代码 |
|---|---|---|
| `kernel` | `kernel_api.cpp`：C ABI 入口与参数检查；`kernel_lifecycle.cpp`：`ayni_start`、`ayni_step`（含 `RUNNING` 中的 Task 派生）、`ayni_begin_shutdown` 的状态转换；`kernel_layout`：按配置容量排布内存块；`ingress/`：有界事件队列；`completion/`：完成槽池（`MA-003`，ADR-0015）；`boot/`：读日志前缀并持久化 `boot_epoch` 的启动序列（`MA-005`，ADR-0016）；`kernel_config.h`：容量、限额与调用方内存的划分；`work_api.cpp`：注册、会话、意图与查询的 C ABI 入口；`intent/`：意图的领域类型、C ABI 解码、`WorkModel`（意图读写的工作表、ID 分配器、transition 计数器、回调标记与派生入口）以及会话、Goal、Task 与实例意图的规则；`transition_clock.h`：transition 的 id 与 `commit_seq`（`MA-004`，ADR-0017、ADR-0018）。GoalType 校验回调运行期间，修改 Kernel 的入口返回 `AYNI_INVALID_STATE`（C-POLICY-001） | M0 |
| `work` | `agent/`：Agent 与驻留实例的暂停、恢复与停止；`goal/`：Goal 树、生命周期、版本、取消级联与触发游标，`goal/trigger_schedule` 推算 occurrence 的到期时间（ADR-0017、ADR-0018、ADR-0019）；`task/`：Task 的生命周期、版本与容量回收；`propagation`：跨 Goal、Task 与实例的放行条件、派生选择与传播（ADR-0018）；调用与动作随后续工作包 | M1（`MA-004`） |
| `authority` | `principal/`：所有者主体与会话表（ADR-0017）；Grant 与 Scene 随 `MA-006` | M1（`MA-004`） |
| `resources` | 无代码 | M1（`MA-018`） |
| `budget` | 无代码 | M1（`MA-018`） |
| `knowledge` | 无代码 | M1（`MA-011`） |
| `catalog` | `catalog_id`：目录 ID 规则；`goal_type/`：GoalType 版本注册与参数校验（ADR-0017）；设备、能力与绑定随 `MA-008` | M1（`MA-004`） |
| `durability` | `batch/`：提交批次的编码与校验，日志前缀的连续性与记录 flags 检查（`storage/records.md`，ADR-0016）；快照、重放与栅栏随 `MA-010` | M1（`MA-005`） |
| `foundation` | `crc32`：CRC-32/ISO-HDLC；`little_endian.h`：持久格式的定长小端读写；`kernel_id.h`：带启动代际的 ID 与分配器（C-ID-010）；`transition_stamp.h`；`bounded_array.h`：调用方内存上的定长数组。状态码只在公开头文件 `include/ayni/ayni.h` 定义一次 | M1（`MA-005`） |

模块在有源文件时才建立构建目标，避免空目标给人“已实现”的印象。`foundation` 承载 ID、时间、有界容器等内部类型；对外状态码仍以公开头文件为唯一来源，不在 Core 内复制一份。

## 内存

Core 不自行分配内存（ADR-0014）。调用方先用 `ayni_kernel_memory_requirements` 查询大小与对齐，再把内存块交给 `ayni_create`；Native 组合根从堆分配，ESP32 组合根从内部 RAM 分配。
