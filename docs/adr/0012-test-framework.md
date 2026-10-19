# ADR-0012：测试框架

- **状态**：已接受（Native 部分在 M0 已实现；ESP32 部分待 M2）
- **日期**：2026-10-19
- **关联**：计划 §10.1、§10.6；`MA-002`、`MA-003`、`MA-024`；AGENTS.md「Testing Guidelines」

## 背景

计划把 GoogleTest 列为候选，要求测试框架不成为设备运行依赖，并要求 E1–E4 先在电脑确定性模拟器通过，再在 ESP32 以同一契约复核。需要决定 Native 与 ESP32 各用什么框架，以及如何让两边执行同一组用例而不写两份。

## 决策

1. **Native 单元与契约测试使用 GoogleTest 1.17.0**，通过 CMake FetchContent 下载官方发布包，并以 SHA-256 锁定（`cmake/AyniGoogleTest.cmake`）。离线构建可用 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 指向本地解压目录。
2. **ESP32 上的测试使用 ESP-IDF 自带的 Unity**，M2 引入，不把 GoogleTest 移植到固件。
3. **用例数据与框架无关。** E1–E4 的场景、注入点与断言写成数据（`contracts/fixtures/`），两边的测试运行器读取同一份数据；断言逻辑放在与框架无关的 C++ 函数中，GoogleTest 与 Unity 只负责调用和报告。这一层在 M1 随第一批 fixtures 实现，M0 只规定格式。
4. **测试命名采用场景名**，例如 `create_rejects_memory_below_requirement`，与 AGENTS.md 一致。
5. **工具脚本的测试使用 Python 标准库 `unittest`**，经 `ctest` 运行，不引入额外的 Python 依赖。
6. **测试框架只链接测试程序**，不进入 Core、runtime、Host 或固件镜像。

## 后果

- 首次配置测试需要网络或预先解压的 GoogleTest；CI 每次下载发布包。
- E1–E4 需要一层薄的“断言函数”，比直接写 GoogleTest 断言多一点代码，换来 Native 与 ESP32 用例同源。
- 覆盖率统计（Core ≥75%、安全关键分支 ≥90%）在 M1 加入，M0 不报告覆盖率数字。

## 备选方案

- **两边都用 Unity**：Native 侧缺少参数化测试、死亡测试与 `ctest` 发现，否决。
- **两边都用 GoogleTest**：需要在固件中启用异常/RTTI 或维护移植，违反 Core 编译约束，否决。
- **Catch2 / doctest**：功能接近 GoogleTest，但计划已列 GoogleTest 为候选，且团队无切换理由，否决。
- **使用系统安装的 GoogleTest**：各发行版版本不同，结果不可复现，否决。

## 验证

- M0：`ctest --preset native-debug` 运行 GoogleTest 单元测试、C99 冒烟测试与工具测试；CI 在 GCC、Clang 与 ASan/UBSan 下运行。
- M1：E1–E4 的第一批 fixtures 由 GoogleTest 运行器读取。
- M2：ESP32 的 Unity 运行器读取同一批 fixtures 并通过（`MA-024`）。
