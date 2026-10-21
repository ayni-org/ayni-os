# 开发流程

- **关联**：`AGENTS.md`；`docs/development.md`；计划 §9、§9.1、§14；`reports/core/m1-progress.md`
- **适用**：接手本仓库工作的任何代理（Claude、Codex 等）与开发者。本文记录 M1 以来实际使用的流程，目的是换人接手时流程与实现保持一致。执行顺序、PR 拆分与当前进度见 `reports/core/m1-progress.md`。

## 1. 依据与优先级

1. 用户在项目讨论中的明确指示，包括对决策卡的选择。
2. `AGENTS.md`：仓库规则（结构、代码风格、测试、提交、架构与安全约束、交付前自查）。
3. `docs/ayni_v1.0.md`：计划。§9.1 给出每个工作包的工作内容、依赖与完成定义；§14 给出已接受的默认值与仍待用户决定的问题。
4. `contracts/`：逐条契约，每条带验证列；`docs/adr/`：已接受的决定。
5. 本文与 `reports/core/m1-progress.md`。

以上相互冲突、或工作要越出用户的要求时，先问用户，不自行扩大范围。

## 2. 一个工作包的流程

1. **读。** 计划 §9.1 中该包的行与依赖；相关契约条目（`contracts/README.md` 的前缀表指向各文件）；前一个相关 ADR；`reports/core/m1-progress.md` 中该包的拆分与已定决定。代码从相邻模块的现有写法学起，沿用同样的命名、注释密度与测试结构。
2. **拆分。** 按依赖把包拆成若干 PR，每个 PR 可以单独审查、单独合并、CI 单独通过。大的包按部分拆（例如 `MA-004` 分三部分共八个 PR，`MA-010` 分 A 至 F 六部分）。用户 2026-10-20 要求提速之后，相互依赖的部分合为一个 PR（`MA-010` 的 B+C+D、E+F）。拆分写进该包第一个 ADR 的决策（例如 ADR-0025 决策 1）和 `reports/core/m1-progress.md`。
3. **区分决策。**
   - **产品语义**（用户看得到的行为、默认值、数据的保留与删除、版本兼容、预算、隐私与安全边界）须请用户决定。提问格式：一个问题；2–4 个选项，每个一句话说明选后会怎样；标出推荐项与理由；问题与选项都用用户的中文。
   - **不等回复。** 提问后按推荐项继续做，并在回复与 ADR 中写明“按推荐项实现，改选再改”；用户改选时修改实现与 ADR。例外：生产环境变更、批量删除、对外发送、超预算采购、开放真实运动等不可撤销或越过计划 §14.2 末段边界的操作，须等用户明确答复。
   - **实施细则**由实现者决定，写进 ADR 的“决策”与“备选方案”，不单独请用户决定。
   - 用户的决定记在 ADR 中，注明“（产品语义，用户选定推荐项）”及日期；影响计划的同时写进计划 §14.1。
4. **实现。** 遵守第 3 节的代码规则。只做该 PR 的范围；发现范围外的问题，写进 ADR 的“后果”或交给后续包，不顺手扩大。
5. **测试。**
   - 以经 C ABI 的场景测试为主（`tests/unit/kernel_*_test.cpp`，用 `tests/unit/kernel_node.h` 的 `Node` 与 `tests/unit/work_node.h` 的 `WorkNode`），每个模块另有自己的单元测试；直接读取内部状态的断言在注释中标明 “White-box”。
   - 测试名描述场景（如 `retried_command_returns_its_first_result`），注释写明它验证的契约条目或 ADR 决策（如 “C-WIRE-004”“ADR-0026 decision 9”），不使用只在讨论里出现过的临时编号。
   - 每条改为已实现的契约，验证列都要引用至少一个已存在的测试（`Suite.test` 形式，`tools/check_contracts.py` 会检查测试是否存在）。
6. **植入缺陷测试（只对核心逻辑）。** Core 中的状态机、准入规则、恢复与提交路径要做；文档、测试、Host 外围与小改动不做。做法：逐个植入 15–50 个小缺陷（反转或放宽比较、删掉一项检查、差一、换掉选择规则），每个重新编译并运行单元测试，记录被至少一个测试发现的个数；没被发现的，要么补测试，要么说明它与原实现行为相同（等价缺陷）并写出理由。结果写进 ADR 的“验证”。
7. **文档，与代码在同一 PR。**
   - **ADR**：每个 PR（或每个合并的 PR 组）一份新 ADR，中文，小节为状态、日期、关联、背景、决策、后果、备选方案、验证；`docs/adr/README.md` 加一行。格式照最近的 ADR（如 ADR-0025、ADR-0026）。
   - **已接受的 ADR 不改写。** 被新 ADR 改变的句子，在句末加括注“（自 ADR-NNNN 起，……）”。
   - **契约**：语义变化同步修改 `contracts/` 中的条款与验证列。验证列以 “M1 已实现（范围）：” 开头并列出测试；只完成一部分时写 “M1 部分实现（范围）：”；尚未完成的部分写 “随 `MA-xxx`”。新增状态码、原因码或限额时同步更新 `contracts/domain/status_codes.md`、`contracts/domain/limits.md`。修改后运行 `python3 tools/check_contracts.py --write` 重新生成 `contracts/traceability.md`。
   - **计划** `docs/ayni_v1.0.md`：文件名不变；版本号加一；更新开头的“当前状态”段；§9.1 的完成定义有变时修改；§14.1 加该版本的修订说明。保留原有中文与标题结构。
   - **进度**：更新 `reports/core/m1-progress.md` 中的 PR、日期与状态。
   - **开发文档**：命令、工具或目录有变时更新 `docs/development.md`。
8. **交付前自查。** 按 `AGENTS.md` 的 “Pre-delivery Design Self-Review”：模块归属与不变量的属主、抽象层次、依赖方向、重复机制、不必要的泛化。
9. **验证。** 运行第 4 节的全部命令。只报告实际运行过的检查及其结果；没运行的写明没运行。
10. **独立审查（只对核心逻辑）。** 由另一个代理或新会话审查本 PR 的完整 diff，按 `AGENTS.md` 的 “Architecture-Aware Review” 自上而下检查，发现要带文件与行号或执行轨迹。逐条修复，或写明不修的理由；修复后重新运行第 4 节的检查。审查的结论与处理写进 PR 正文的 Review 小节。
11. **PR。** 标题 `area: imperative summary (MA-xxx)`，英文；正文英文，依次为 Before、After、How、Validation、Review、Limitations 六个小节。Validation 写出实际运行的命令与结果，Limitations 写出未做与未验证的部分。
12. **CI 与合并。** CI 的 8 项中，除仅报告的 clang-tidy 外都必须通过（见第 4 节）；CI 失败时找到根因再修，不重跑碰运气，不跳过或禁用测试。M1 的 PR 在 CI 全绿且独立审查的发现处理完之后合并，方式为 merge commit（不 squash、不 rebase）。Claude 的项目讨论中由 Claude 自行合并（用户已授权）；其他代理按用户的指示合并或请用户合并。
13. **合并之后。** 用中文向用户汇报：改了什么、验证与审查结果、需要用户做什么（没有就写没有）、下一步。开发分支重置到最新 main：`git fetch origin main && git checkout -B <分支> origin/main`，再 `git push --force-with-lease`。删掉审查用的临时 worktree。

## 3. 代码规则摘要

完整规则见 `AGENTS.md`，以下是 M1 中反复用到的部分。

- **Core**：C++17，不用异常、RTTI 与堆分配（ctest `core_does_not_reference_heap_allocator` 检查，ADR-0014）；全部存储来自调用方提供的内存，容量由 `ayni_config_v1` 给出并有上限；`ayni_step` 有界，从不等待 I/O。
- **分层**：`kernel` > `work`、`authority` 等 > `catalog` > `durability` > `foundation`；`tools/architecture.json` 定义，`tools/check_deps.py` 检查。Core 不依赖 Host 或 runtime。
- **公共 ABI**：C99，`include/ayni/ayni.h`。任何 ABI 变化（新字段、常量、结构大小或语义）都要把 `AYNI_ABI_MINOR` 加一，在相关注释中写 “Since 0.N”，同步 `tests/contract/abi_layout.c` 与 `tests/contract/c_abi_smoke.c`；按 C-VER-006，0.x 的次版本须完全匹配，旧调用方须重新编译。ADR 中写明内存需求的变化（默认配置与最小工作容量的字节数）。
- **状态码与原因码**：每个原因码只属于一个状态码（`contracts/domain/status_codes.md`）。
- **持久记录**：定长小端编码，不含单调时间、指针、句柄与资源占用（C-STORE-014）；布局记在 `contracts/storage/records.md`。
- **格式**：clang-format 18（`.clang-format`，列宽 100）；注释约 78 列，解释不变量、所有权与设计理由，不复述代码。
- **提交**：`area: imperative summary`（如 `core: journal each transition and replay the log at recovery`），正文说明原因；一个提交只做一件事。提交、PR 与代码中不写模型名。
- **不进入版本库**：`.claude/`、构建目录、凭据、私人媒体（包括开发板照片）。

## 4. 验证命令

本地依次运行，与 CI 一致：

```sh
for preset in native-debug native-release native-asan native-tsan; do
  cmake --preset "$preset" && cmake --build --preset "$preset" && ctest --preset "$preset" || break
done
CC=clang CXX=clang++ cmake -S . -B build/native-clang -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native-clang && ctest --test-dir build/native-clang --output-on-failure
./build/native/targets/simulator/ayni-sim
python3 tools/check_deps.py
python3 tools/check_contracts.py
git ls-files '*.c' '*.cpp' '*.h' | xargs clang-format-18 --dry-run --Werror
git diff --check origin/main
```

- TSan 无法启动时先执行 `sudo sysctl vm.mmap_rnd_bits=28`（见 `docs/development.md`）。
- 仓库内有代理的 worktree（例如 `.claude/worktrees/`）时，`tools/check_deps.py` 会把其中的源文件也算进来而报错；这不是真正的失败，删掉 worktree 后再运行。
- CI（`.github/workflows/ci.yml`）的 8 项：依赖、契约与格式检查；GCC 的 native-debug、native-release、native-asan、native-tsan；Clang 的 native-debug；clang-tidy（仅报告，不阻断）；ESP32-S3 Hello 固件构建。

## 5. 与用户沟通

- 用中文，先说结论；每条回复只写比上一条新的内容，说明需要用户做什么，没有就说没有。
- 结论附证据（测试、文件与行号、CI 结果）；推断要写明是推断。
- 不报告没有运行的检查；不把模拟器结果写成硬件结果。
- 需要用户决定时用第 2 节第 3 步的格式提问，一次只问必要的问题。

## 6. 安全与硬件约束

- 不执行任何 burn 类命令，不烧写 eFuse；查看 eFuse 只用只读的 summary 命令（`docs/development.md`）。
- 开发板相关的实机操作（烧录 Hello、确认 Flash 与 PSRAM、读取 eFuse 摘要）须在用户的 Linux 电脑上、经用户允许后进行。
- 不关闭 TLS 校验；凭据只在运行时配置，不写入提交或日志。

## 7. 接手

用户 2026-10-21 要求 Claude 做完 `MA-010` B+C+D（#20）后停止，M1 的其余工作交给其他大模型。接手方按第 2 至 6 节的流程继续，并注意以下几点。

1. **读的顺序。** `AGENTS.md`；本文；`reports/core/m1-progress.md` 的“交接状态”“剩余工作”“已作的决定”“需要用户的事项”；计划 §9.1 中下一个包的行与依赖，以及 §14；该包涉及的 ADR 与契约（`MA-010` E+F 先读 ADR-0025、ADR-0026 与 `contracts/storage/records.md`）；最后读相邻的代码与测试。
2. **分支。** 从最新 main 开自己的分支（`git fetch origin main && git checkout -b <分支> origin/main`），PR 合并后按第 2 节第 13 步重置。不推送到其他代理的分支（如 `claude/project-thread-6xckzw`），不改写别人分支的历史。合并按第 2 节第 12 步。
3. **独立审查。** 在新会话或另一个代理中做，只读，发现带文件与行号或执行轨迹。审查用的 worktree 放在仓库目录之外（如 `git worktree add ../ayni-review <分支>`）；放在仓库内会让 `tools/check_deps.py` 报错（第 4 节）。审查结束后用 `git worktree remove` 删掉。
4. **植入缺陷测试的做法。** 用一个脚本逐项处理缺陷清单，每项为文件、原文片段与缺陷片段：
   - 原文片段在文件中必须恰好出现一次，否则跳过并记下；
   - 替换后只编译单元测试目标：`cmake --build --preset native-debug --target ayni_unit_tests`；编译失败的不计入总数；
   - 运行 `./build/native/tests/unit/ayni_unit_tests --gtest_brief=1`，非零退出即为“被发现”，记下失败的测试名；
   - 无论结果如何，都用脚本保存的原文写回文件（不用 `git checkout`，以免丢掉未提交的改动）；全部做完后重新编译一次。

   脚本与原始结果放在仓库之外，不提交。总数、发现数与每个未被发现的缺陷的理由写进 ADR 的“验证”（格式见 ADR-0026）。
5. **内存数字。** ABI 或容量变化后，用一个小 C 程序调用 `ayni_kernel_memory_requirements` 量两组数字，与上一版比较后写进 ADR 与 PR 正文：
   - 默认配置：`ayni_config_v1` 只填 `abi_major` 与 `abi_minor`，`struct_size` 设为 12，只覆盖这三个字段，其余取默认值；
   - 最小工作容量：`struct_size` 为整个结构，各工作容量取其 `_MIN` 或 1，`command_capacity` 取 1（结构覆盖该字段时 0 无效），其余取默认值。
6. **提问。** 产品语义问题仍按第 2 节第 3 步提问，并按推荐项继续。没有决策卡时，在回复或 PR 正文中用同样的格式提问，ADR 中写明“按推荐项实现，改选再改”。
