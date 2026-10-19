# ADR-0014：Core 不自行分配内存

- **状态**：已接受（`ayni_create` 部分在 M0 已实现）
- **日期**：2026-10-19
- **关联**：计划 §3.2、§4.2、§8；`MA-002`、`MA-005`；ADR-0006、ADR-0010

## 背景

计划要求 Core 使用有界存储、显式容量和可失败分配，并要求 ESP32 区分内部 RAM 与 PSRAM：停止链与关键任务的数据必须在内部 RAM，大缓冲可以放 PSRAM。若 Core 自己调用 `malloc` 或 `new`，内存放在哪里就由 Core 决定，而 Core 不能依赖 ESP-IDF 的 `heap_caps_*`。§3.2 的草案中 `ayni_create` 只有输出句柄，没有说明内存来源。

## 决策

1. **Kernel 放在调用方提供的内存中。** 调用方先调用 `ayni_kernel_memory_requirements(config, &size, &align)`，再把一块满足大小与对齐的内存交给 `ayni_create`。对齐值保证是 2 的幂。
2. **内存块归调用方所有。** `ayni_destroy` 返回 `AYNI_OK` 之前，调用方不得使用或释放它；`ayni_create` 失败时不写入这块内存。
3. **容量由配置决定。** M1 起，`ayni_config_v1` 增加各类容量上限（实例、任务、队列、日志等），`ayni_kernel_memory_requirements` 按配置算出总需求；Kernel 运行期间不再申请内存，容量用尽返回 `AYNI_CAPACITY_EXCEEDED`。
4. **由组合根选择内存来源。** `runtime/native_node` 从堆分配；`runtime/esp_idf_node` 用 `heap_caps_aligned_alloc` 从内部 RAM 分配。
5. **Core 只包含白名单中的标准头文件**（`tools/architecture.json` 的 `core_std_headers`），其中不包含 `<memory>`、`<vector>` 等会分配内存的容器头文件。

## 后果

- 内存占用在启动时即可知，ESP32 的内部 RAM 预算可以在 M2 前估算。
- ABI 比草案多一个函数和两个参数，调用方多一步查询。
- 将来如需按需增长的结构（如更大的检索索引），只能放在 Provider，不放在 Core。
- 白名单检查只看直接包含；通过允许的头文件间接引入分配的情况，由代码评审和 ASan 构建发现。

## 备选方案

- **Core 内部调用 `malloc`**：无法区分内部 RAM 与 PSRAM，且分配失败散落在各处，否决。
- **在 `ayni_host_v1` 中注入分配器函数**：仍允许运行期分配，容量无法在启动时确定，否决。
- **由编译期常量固定容量**：同一份 Core 无法同时满足 `CORE_LITE` 与 `CORE_FULL` 的不同容量，否决。

## 验证

- `tests/unit/kernel_api_test.cpp`：内存不足返回 `AYNI_BUFFER_TOO_SMALL`，对齐不足返回 `AYNI_INVALID_ARGUMENT`，失败时内存块保持原样，同一块内存可反复创建与销毁。
- `check_deps_repository`：Core 只包含白名单中的标准头文件。
- M2：报告 ESP32 上 Kernel 在内部 RAM 中的实际占用。
