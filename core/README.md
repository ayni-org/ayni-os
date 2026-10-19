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
| `kernel` | `kernel_api.cpp`：ABI 版本查询、内存需求、`ayni_create`、生命周期查询、`ayni_destroy`、`ayni_completion_post`；`kernel_layout`：按配置容量排布内存块；`ingress/`：有界事件队列；`completion/`：完成槽池（`MA-003`，ADR-0015） | M0 |
| `work` | 无代码 | M1（`MA-004`） |
| `authority` | 无代码 | M1（`MA-006`） |
| `resources` | 无代码 | M1（`MA-018`） |
| `budget` | 无代码 | M1（`MA-018`） |
| `knowledge` | 无代码 | M1（`MA-011`） |
| `catalog` | 无代码 | M1（`MA-005`、`MA-008`） |
| `durability` | 无代码 | M1（`MA-010`） |
| `foundation` | 无代码；状态码只在公开头文件 `include/ayni/ayni.h` 定义一次 | M1（`MA-005`、`MA-010`：CRC 与 ID 类型） |

模块在有源文件时才建立构建目标，避免空目标给人“已实现”的印象。`foundation` 将来承载 ID、时间、有界容器等内部类型；对外状态码仍以公开头文件为唯一来源，不在 Core 内复制一份。

## 内存

Core 不自行分配内存（ADR-0014）。调用方先用 `ayni_kernel_memory_requirements` 查询大小与对齐，再把内存块交给 `ayni_create`；Native 组合根从堆分配，ESP32 组合根从内部 RAM 分配。
