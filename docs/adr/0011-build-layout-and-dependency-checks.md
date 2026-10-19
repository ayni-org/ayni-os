# ADR-0011：构建布局与依赖检查

- **状态**：已接受（M0 已实现，证据见 `reports/core/m0-review.md`）
- **日期**：2026-10-19
- **关联**：计划 §2.0、§2.3、§8、§11；`MA-002`；ADR-0008、ADR-0010

## 背景

ADR-0008 与 ADR-0010 规定了 Core 九个模块的依赖方向、`runtime/` 是唯一的跨组件装配位置、`targets/` 只调用 `runtime/`，并要求“实现开始后在 CI 中自动检查”。M0 开始写代码，需要决定这些规则写在哪里、由什么机制强制，以及 Core 如何同时进入 Native CMake 工程和 ESP-IDF 工程。

## 决策

1. **规则只声明一次。** Core 层次、子目录规则、Core 允许的标准头文件和各顶层目录可包含的头文件种类都写在 `tools/architecture.json`。CMake 与检查脚本读同一个文件，二者不会各写一份而出现分歧。
2. **两道检查，各管一类依赖。**
   - 链接依赖：`cmake/AyniLayers.cmake` 的 `ayni_add_core_module` 在配置阶段拒绝指向同层或更高层的 `DEPENDS`。
   - 包含依赖：`tools/check_deps.py` 扫描 `#include`，按目录套用规则，并解析相对路径，避免用 `../../core/...` 绕过。
   二者都有测试（`tests/tools/`），并在 CI 中运行。
3. **Core 内部包含根不外泄。** Core 模块以 `PRIVATE` 方式链接 `ayni_core_internal`（包含根 `core/`），所以链接 `ayni_core` 的组件只看得到 `include/ayni/` 中的公开 API。
4. **头文件分为四类。** `ayni/`（已实现的公开 ABI）、`ayni/draft/`（计划中的接口，只做编译检查，没有符号）、`ayni/hosts/<名称>`、`ayni/runtime/<名称>`。规则如下：
   - Core 只能包含公开 ABI 与白名单标准头文件；
   - Host 只能包含公开 ABI 与自身头文件；
   - `runtime/` 可以包含任何 Host，但不能包含其他 runtime；
   - `targets/` 只能包含公开 ABI 与 runtime；
   - `control/`、`safety/`、`examples/` 不能包含 Core 内部、Host 或 runtime 头文件；
   - 没有登记规则的顶层源码目录直接报错，迫使新目录先声明规则。
5. **`targets/` 可以直接调用公开 C API 的查询函数**（如 `ayni_get_lifecycle`）。ADR-0010 禁止的是目标程序自行装配组件；为每个查询再包一层 runtime 函数只会制造空转发层。
6. **Core 的 `CMakeLists.txt` 自包含。** Native 工程与 ESP-IDF 的 runtime 组件都通过 `add_subdirectory(core)` 引入同一份 Core，编译选项来自 `cmake/AyniCommon.cmake`。
7. **runtime 子目录按节点命名**：`runtime/native_node/`、`runtime/esp_idf_node/`。ESP-IDF 以目录名作组件名，若沿用 Host 的 `native`、`esp_idf` 会与 `hosts/esp_idf/` 同名冲突。
8. **编译设置。** C99 与 C++17，均不启用编译器扩展；只支持 GCC 与 Clang，其他编译器在配置时报错。Ayni 源文件启用 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`，默认 `-Werror`（`ayni_warnings`）。包含 ESP-IDF 头文件的源文件（ESP-IDF Host、runtime 与固件入口）去掉 `-Wpedantic`（`ayni_platform_warnings`），因为这些厂商头文件使用了 GCC 扩展；Core 在 ESP-IDF 构建中仍使用完整告警。Core 另加 `-fno-exceptions -fno-rtti`。
9. **格式与静态分析。** clang-format 18 的检查是阻断项；clang-tidy 在 M0 只报告、不阻断，现有发现清零后再改为阻断项。

## 后果

- 新增模块依赖时，必须先改 `tools/architecture.json` 并通过评审，代码中的越层包含会在本地测试和 CI 中失败。
- 模块只有在有源文件时才建立构建目标；M0 只有 `kernel` 有目标，其余八个模块在 `core/README.md` 中登记状态。
- ESP-IDF 以 `gnu++26` 编译同一份 Core，Native 以 C++17 编译；C++17 的可移植性由 Native 构建保证。
- 检查脚本基于文本解析 `#include`，不展开宏包含（如 `#include MACRO`）。Ayni 源码不使用宏包含；若将来需要，须先扩展检查。

## 备选方案

- **只用 CMake 目标依赖**：头文件包含不受链接图约束，模块可以包含未声明依赖的头文件，否决。
- **只用 include 检查脚本**：链接图仍可能出现环，且无法在配置阶段给出错误，否决。
- **在每个模块目录放规则文件**：规则分散，CMake 需要逐个读取；ADR-0008 要求的“在模块内声明”改为在单一文件中按模块分组登记，否决分散文件。
- **为每个 Core 模块预先建立空构建目标**：会制造没有代码的目标，并诱导提前声明不存在的依赖，否决。

## 验证

- `ctest` 中的 `check_deps_rules`（22 个场景）、`check_deps_repository` 与 `layers_*`（5 个配置用例）。
- CI 的 `checks` 任务运行依赖检查与格式检查，`native` 任务以 GCC 与 Clang 构建并运行全部测试，`esp-idf` 任务构建 Hello 固件。
