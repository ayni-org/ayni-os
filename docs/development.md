# 开发环境与命令

本文记录 M0（`MA-002`）及 M1 实际使用的环境与开发命令。每个 PR 的按需本地验证、适用 CI、独立审查和合并流程以 [`workflow.md`](workflow.md) 第 2、4、7 节为准；实际执行结果记在对应 ADR。开发环境以 Linux 为准；macOS 未经验证，不在此列出步骤。

当前按[基础通用能力实施与实验计划](architecture/foundation-and-experiment-plan.md)先完成基础，再安排 ESP32-S3、Linux 系统等实验。下列 Native/Sim 开发命令与现有 CI 继续使用；它们不等同于后续 Linux 实际 OS 服务集成实验。ESP-IDF 构建是已有实验入口，不要求所有部署具备外设，也不由本次文档修订启动板级操作或新专项。

## 已验证的版本

| 工具 | 版本 | 用途 |
|---|---|---|
| Ubuntu | 24.04.5 LTS，x86_64 | 本地；CI 用 `ubuntu:24.04` 容器（见“CI 机器”） |
| CMake | M0 / CI 工具镜像：3.28.3；早期 M1 容器：3.31.6（最低要求 3.22） | Native 构建 |
| Ninja | 1.11.1 | CMake 预设使用的生成器 |
| GCC | 13.3.0 | Native 构建，含 ASan/UBSan 与 TSan |
| Clang | 18.1.3 | Native 构建 |
| clang-format / clang-tidy | 18.1.3 | 格式检查（阻断）与静态分析（仅报告） |
| Python | 最低 3.9；M0 本地：3.13.16；M1 容器：3.12.3 | `tools/check_deps.py`、`tools/check_contracts.py` 与其测试 |
| SQLite | 最低 3.31；本地 Ubuntu 包 3.45.1 | Native 会话消息/原始结果；Core 不依赖 |
| libcurl | 最低 7.85；本地/CI Ubuntu 包 8.5.0 | Native Chat Completions，须支持线程安全初始化与异步 DNS |
| yyjson | 0.10.0，按 SHA-256 锁定（MIT） | Provider 有界 JSON；不进入 ESP-IDF |
| GoogleTest | 1.17.0，按 SHA-256 锁定 | Native 单元测试（ADR-0012） |
| ESP-IDF | v6.0.3，工具链 `esp-15.2.0_20251204` | ESP32-S3 固件；CI 使用镜像 `espressif/idf:v6.0.3` |

M0 本地 Clang 缺少 ASan 运行库（`libclang-rt-18-dev`），当时只验证普通 Debug 构建；当前本地按改动选择配置，CI 分工见 workflow 第 4 节。M1 本轮使用本机已有的 `ayni-m0-tools:latest` 容器，源码挂载到 `/workspace`，`CC=gcc-13`、`CXX=g++-13`；容器内实际版本为上表所列 CMake 3.31.6、GCC 13.3.0、Clang/clang-format 18.1.3、Ninja 1.11.1、Python 3.12.3。仓库没有该镜像的构建配方，复现以表中工具版本、预设和逐 PR 验证记录为准。

## 本轮验证的资源限制

用户 2026-10-21 明确：先检查资源，再决定构建并行度，不统一要求串行。构建或测试前检查宿主可用内存、swap 压力、CPU 与其他任务，把其他代理和容器的峰值占用一并计入预算，给桌面及其他应用留余量。按预算设置明确的 `CMAKE_BUILD_PARALLEL_LEVEL`、测试并行度 `CTEST_PARALLEL_LEVEL` 及容器 CPU/内存上限；资源允许时可并行，压力上升时降低并行度或暂停，不使用无界 `-j`，也不只按 CPU 核数决定任务数。`MA-010` E+F 崩溃后的单任务、1 CPU、2 GiB 验证配置是当次保守设置，不是后续固定要求。用户 2026-10-28 已授权恢复完整 E2 → E1 → E3 → E4 所需运行器、故障注入与恢复专项，见[完整实验实施计划](../reports/core/m1-experiment-plan.md)；变异专项继续暂停。现有测试仍在 CI 完整套件中运行。

## Linux 准备

```sh
sudo apt-get install -y build-essential clang cmake ninja-build python3 \
    clang-format-18 clang-tidy-18 git libcurl4-openssl-dev libsqlite3-dev
```

## Native 构建与测试

以下命令按需选择，并非每个 PR 必跑步骤。本地可只构建相关目标、用 `ctest -R` 或 GoogleTest filter 运行相关场景；完整现有套件由普通代码 PR 的 CI ASan/UBSan 承担。预设定义在 `CMakePresets.json`：

| 预设 | 构建目录 | 内容 |
|---|---|---|
| `native-debug` | `build/native` | Debug，含测试 |
| `native-release` | `build/native-release` | Release，含测试 |
| `native-asan` | `build/native-asan` | Debug，启用 AddressSanitizer 与 UndefinedBehaviorSanitizer |
| `native-tsan` | `build/native-tsan` | Debug，启用 ThreadSanitizer，覆盖入口队列与完成槽的多线程测试（ADR-0015） |

GCC 13 自带的 ThreadSanitizer 在 mmap 随机化为 32 位（`/proc/sys/vm/mmap_rnd_bits`）的内核上可能报 `unexpected memory mapping` 而无法启动，这时先执行 `sudo sysctl vm.mmap_rnd_bits=28`；CI 机器把它固定为 28，TSan 任务开始前检查（见“CI 机器”）。M1 本轮先读到宿主值为 32，已按 workflow 在运行 TSan 前设置为 28；容器共享宿主内核，镜像本身不会改变这个参数。

```sh
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug
./build/native/targets/simulator/ayni-sim
```

不使用预设时，计划 §11 中的命令同样有效：

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

换用 Clang：在配置前设置 `CC=clang CXX=clang++`，并用 `-B` 指定另一个构建目录。只有带版本号的可执行文件时，使用对应的 `CC=clang-18 CXX=clang++-18`；GCC 同理可用 `CC=gcc-13 CXX=g++-13`。`AYNI_HOST` 目前只接受 `native`；ESP32 固件用下文的 `idf.py` 构建。`ayni-sim` 无参数时保留原演示；正常夹具入口见下节，完整 profile 已提供 `--experiment HARNESS`、`--experiment E2`、`--experiment E1`、`--experiment E3` 与 `--experiment E4`，均须加 `--output NEW_DIRECTORY`；精确切断和版本探针须启用 `AYNI_BUILD_TESTS`。无参数演示每次运行在系统临时目录（`TMPDIR`，默认 `/tmp`）下新建状态目录，Native Host 的日志文件写在其中，Kernel 完成恢复并停机后删除该目录。Native Host 的单元测试与 Host 存储契约测试同样在临时目录中创建并删除存储文件，所以运行它们需要可写、支持 `fdatasync`、文件锁、rename 和目录同步的临时目录。状态目录包含 `commit_log`、双槽文件 `snapshot_0` / `snapshot_1` 及稳定锁文件 `.ayni_storage_lock`，轮转还会产生临时输出；保留设备状态时须保留整个目录。当前批次与快照只读写 schema 8；schema 0..7 及其他不支持的 schema 在新写入前只读受限并保留，不迁移、不清空、不回退旧快照（ADR-0082）。开发使用新的状态目录；回退需原二进制与原状态目录。

ESP-IDF 存储工作恢复时，需量测 schema 3 下典型操作的实际日志写入字节数、达到快照/轮转阈值的操作次数及快照写入量，并结合目标 Flash 的擦除块、擦写次数和写入策略评估代价。记录正文的固定部分长度不等于整次操作的写入量，变长字段、批次封装和快照也须计入；当前尚无这组测量，不以 Native arena 数字代替 Flash 耐久证据。此项随 ESP-IDF 存储验证进行，不启动 M2 或硬件工作。

### 测试并行与编译缓存

`ctest` 默认一次只跑一个用例，而 `gtest_discover_tests` 把每个 GoogleTest 用例注册成一个独立进程，串行时进程启动占了大部分时间，sanitizer 预设下尤其明显。按上文的资源预算设置 `CTEST_PARALLEL_LEVEL` 即可并行。用例的存储文件各在 `mkdtemp` 新建的临时目录里，彼此不共享文件。2026-10-23 限制在 2 个 CPU 上实测：`native-asan` 由 77 s 降到 40 s，`native-tsan` 由 39 s 降到 19 s，并行数 4 不比 2 更快。预设不写死并行数，因为合适的值取决于当时的资源预算。

单元测试目标 `ayni_unit_tests` 用预编译头：每个测试文件都 include 的 `gtest.h` 解析一次约 0.9 s，现在连同几个常用标准库头每次构建只解析一次。测试文件仍应 include 自己用到的头；每周的 clang-tidy 运行关掉预编译头构建，以发现漏掉的 include，本地也可以在配置时加 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 检查。Release 下测试代码按 `-O1` 编译，Core 仍是 `-O3`：测试只负责驱动 Core，按 `-O3` 优化测试代码只增加构建时间。

装了 ccache 时，在首次配置前设置 `CMAKE_C_COMPILER_LAUNCHER=ccache` 与 `CMAKE_CXX_COMPILER_LAUNCHER=ccache`（环境变量或 `-D` 均可），之后没有变化的文件直接取缓存。因为有预编译头，还要设置 `CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime`，否则 ccache 不缓存单元测试。编译带 `-g` 时 ccache 把工作目录（即构建目录）算进缓存键，所以构建目录换了路径不会命中。

用户 2026-10-24 最新决定替代 F 的本地验证门槛：本地只跑相关检查，不强制完整套件或五配置；普通代码 PR 在 CI 跑 ASan/UBSan，2026-10-25 进一步限定附加配置仅通过显式 `ci:full`、手动或周期触发。复用固定构建目录、ccache 与 PCH，相关验证与只读审查可重叠。阶段收尾引用适用的最终代码结果或补跑手动 CI，不重复要求本地五配置。

### CI 机器

CI 全部在自托管 runner 上跑（用户 2026-10-23 决定）：一台专门跑 CI 的机器 `danny-cipc`，32 个 CPU、约 7.2 GiB 内存与 4 GiB swap，上面有两个 runner 实例（用户 2026-10-26 告知），共用一个 Docker daemon，以同一个无 sudo 的用户运行并开机自启。两个 runner 同时各接一个 job；每个 Native 容器 12 CPU、3 GiB，两个容器合计 6 GiB，给宿主留出约 1 GiB。构建并行度 6（编译占内存），测试并行度 12（测试主要等进程启动），TSan 测试在 `native.sh` 中限为 6；`native.sh` 结束时打印容器的内存峰值（cgroup v2 `memory.peak`），调整并行度或限额时以它为准。宿主提供 Git、Docker、Bash 与 `flock`，Native 工具来自 Ubuntu 24.04 包（GCC 13.3.0、Clang/clang-format 18.1.3、CMake 3.28.3、ccache 4.9.1），版本与宿主系统无关；固件用 `espressif/idf:v6.0.3`。

检查、Native 构建与 clang-tidy 共用本机 `ayni-ci-tools:<配方键>` 镜像。`tools/ci/run_tools.sh prepare` 以基础镜像名和 `tools/ci/install_tools.sh` 内容生成缓存键：首次或镜像缺失时，在受限容器里安装一次，只有安装成功才发布本地镜像；后续直接复用。更新工具包或安全补丁时修改安装配方的 revision，触发重新安装。镜像保留在 runner 的 Docker daemon 中，不上传注册表；新 runner 或清除镜像后会重建。基础镜像标签与 apt 仓库允许更新，此缓存用于避免重复安装，不声称锁定每个包的精确版本；Native job 会打印实际工具版本。

Native 命令通过 `tools/ci/run_tools.sh run <命令> <参数…>` 执行，每个容器限制 12 CPU、3 GiB 内存并禁止额外 swap，两个 runner 同时运行时仍在机器的预算内。镜像初始化也使用同样限制；失败会清理安装容器，不复用半成品。镜像初始化用本机用户级 `flock` 串行，两个 runner 以同一用户运行，所以共用一把锁。源码在每个容器里都挂载到 `/__w/ayni/ayni`：路径属于容器，两个 job 不冲突，Debug 的 ccache 键也保持不变；命令以 runner UID/GID 运行，取消也不会留下 root 属主的源码文件；Docker init 转发取消信号并回收子进程。checkout 和资源检查在宿主执行；普通代码 PR 的轻量检查与完整 ASan/UBSan 通过 `tools/ci/checks.sh` 在同一容器运行。附加 Release/Clang/TSan 是矩阵中的独立 job（`tools/ci/select_checks.py` 输出 `native_presets`），每个 job 一个容器运行 `tools/ci/native.sh <预设>`，两个 runner 并行取用；ESP-IDF job 同样与它们并行。

编译缓存与 GoogleTest 发布包继续使用 Docker 卷 `ayni-ccache`、`ayni-ccache-esp-idf`、`ayni-downloads`，跨运行保留，两个 runner 共用：ccache 支持多个进程同时读写，共用比各自一份命中率更高。Native ccache 上限 4 GiB，容纳四个 Native 配置（原 1 GiB 已满）。`prepare` 只接管 root 属主的卷（旧的 root job 容器或新建的卷），属于另一个非 root 用户时报错退出：两个 runner 若以不同用户运行，每次接管都会打断另一个 runner 正在使用缓存的 job。ESP-IDF job 仍用原 job container 和缓存，最后把 root 创建的文件归还 runner。

机器的一次性准备要 sudo，之后运行 CI 不需要：

- 安装 Docker 与 Git；把 runner 的用户加进 docker 组（`sudo usermod -aG docker <用户>`），然后重启 runner 服务；
- 在 `/etc/sysctl.d/99-ayni-tsan.conf` 写入 `vm.mmap_rnd_bits=28` 并执行 `sudo sysctl --system`：容器不能改宿主内核参数，TSan 任务开始前检查这个值；
- 磁盘至少留 40 GB，ESP-IDF 镜像约 16 GB。

工具镜像可用 `docker image ls ayni-ci-tools` 查看，再用 `docker image rm <具体镜像标签>` 删除；下次运行会自动重新安装。清空编译与下载缓存用 `docker volume rm ayni-ccache ayni-ccache-esp-idf ayni-downloads`。`checks and ASan` 任务的 “Runner resources” 一步打印 CPU、内存、磁盘、内存压力与活动容器；TSan 步骤用 `tools/ci/run_tools.sh mmap-bits` 读取宿主 mmap 随机化位数（该文件可能只有 root 可读，因此只对这次无挂载的读取使用容器 root），换机器或调并行度时先看这里。哪些事件跑哪些任务见 `docs/workflow.md` 第 4 节及 `tools/ci/select_checks.py`：日常所有代码 PR（包括并发、公共头、ESP、CMake 和 CI 修改）只跑检查及一套 GCC Debug + ASan/UBSan；不再按路径追加其他配置。Release、Clang、TSan 与 ESP-IDF 仅通过显式 `ci:full`、定期或手动运行。

### 离线配置 GoogleTest

首次配置测试会下载 GoogleTest 发布包，每个构建目录各下载一次。要让多个构建目录共用一份，在配置时用 `-DAYNI_DOWNLOAD_DIR=<目录>` 指定同一个目录：其中已有、哈希匹配的包不再下载。没有网络时，先把 `googletest-1.17.0.tar.gz` 解压到本地，再在配置时指定：

```sh
cmake --preset native-debug -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.17.0
```

### Native 模型 Provider

Native 构建增加 `libcurl4-openssl-dev` 与 `libsqlite3-dev`；CI 安装配方已包含。yyjson 由
`cmake/AyniYyjson.cmake` 固定版本/哈希，复用 `AYNI_DOWNLOAD_DIR`。离线配置可指定
`-DFETCHCONTENT_SOURCE_DIR_YYJSON=/path/to/yyjson-0.10.0`，与 GoogleTest 相同。
ESP-IDF 不引入 curl 或 yyjson。相关本地用例可用 `ctest -R Openai` 选择；
测试只连回环 HTTP，不读取实服务密钥。手动实 API 程序显式构建、显式 `--live`，
不注册为 CTest，运行方法见 [Provider 说明](../providers/openai_compat/README.md)。

## 依赖、契约、格式与静态分析

```sh
python3 tools/check_deps.py                                   # 依赖规则（ADR-0011）
python3 tools/check_contracts.py                              # 契约条款与追踪矩阵（ADR-0013）
python3 tools/check_readability.py                            # 可读性的机械规则与遗留基线
git ls-files '*.c' '*.cpp' '*.h' | xargs clang-format-18 --dry-run --Werror
run-clang-tidy-18 -p build/native -quiet 'core/|hosts/|runtime/|targets/'
git diff --check
```

`ctest` 也会运行依赖、契约、可读性机械规则检查及其测试，并检查每个 Core 静态库不引用堆分配函数（ADR-0014）。修改 `contracts/` 中的条款后运行 `python3 tools/check_contracts.py --write` 重新生成 `contracts/traceability.md`，否则检查失败。clang-tidy 只报告，不阻断 CI，只在每周一的定时运行和手动触发时运行。

### 可读性检查的边界

`check_readability.py` 只做四项机械检查：Core 中超过 80 行的具名函数、同目录同文件名的头源文件合计超过 150 行且注释行不足 5%、ID 辅助头外的两类原始位运算模式，以及没有相邻注释的公开状态/原因码。行数包括空行；注释按物理行计数，行尾注释也算。函数从声明首行计到函数体末行，包含内部 lambda；独立 lambda、宏生成的函数和语义判断不在识别范围。脚本按词法读取所有条件编译分支，不替代编译器；不平衡语法会明确报错，需审查后完善识别规则。模板默认实参中的比较/移位表达式应加括号，避免词法扫描把 `<` 误作模板层级；无法配平时检查失败，不静默略过函数。

通过表示没有新增或加重已识别的机械违例，不代表可读性审查通过。职责是否清楚、命名是否准确、抽象是否必要、是否重复表达业务规则，以及注释是否解释了不变量，仍须按 workflow 独立审查。注释比例只是发现缺少说明的线索，不能通过增加无用注释来满足要求；公开码的含义与 `Since 0.N` 内容也由审查核对。

`python3 tools/check_readability.py --list` 列出现存问题。`tools/readability_baseline.json` 按规则、路径、声明或表达式记录历史债务，不依赖行号；问题消除后须同时删除对应项。非 Kernel ID 的位运算只能用带理由、精确表达式和次数上限的例外排除。CI 不生成基线；`--write-baseline` 仅供显式记录现状，并须逐项审查 diff，不能用来绕过新违例。

## ESP32-S3 固件

当前 Hello 编译仍带入共享 Core，可检查目标平台编译与链接。日常 PR 不自动触发编译；按用户 2026-10-25 决定，仅随周期或显式完整验证运行。镜像和 ccache 继续复用。

安装锁定版本的 ESP-IDF（只需一次）：

```sh
git clone --branch v6.0.3 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
```

每个终端先激活环境，再构建：

```sh
. ~/esp/esp-idf/export.sh
export IDF_PY_BUILD_JOBS="$CMAKE_BUILD_PARALLEL_LEVEL"
cd targets/esp32_assistant
idf.py build
idf.py size
```

构建前仍须按本节资源预算设置正整数 `CMAKE_BUILD_PARALLEL_LEVEL`。
ESP-IDF v6.0.3 的 `idf.py` 通过 `IDF_PY_BUILD_JOBS` 将该预算传给 Ninja；
只设置 CMake 的并行变量不足以限制 `idf.py` 直接启动的 Ninja。

`sdkconfig.defaults` 已指定 `esp32s3` 与 8 MB Octal PSRAM，因此首次构建无需 `idf.py set-target`。Flash 暂按 2 MB 配置：镜像头声明的容量大于芯片实际容量时固件在启动时停下，来不及打印基线日志；2 MB 是所有 ESP32-S3 模组的最小容量，Hello 固件会打印实测容量，到板实测后再改为实际容量并确定分区。生成的 `sdkconfig` 与 `build/` 不进入版本库。

烧录与串口监视（板子到手后执行，M0 尚未执行）：

```sh
idf.py -p <SERIAL_PORT> flash monitor
```

Hello 固件启动后打印芯片型号与版本、ESP-IDF 版本、复位原因、实测 Flash 与 PSRAM 容量、内部 RAM / DMA / PSRAM 堆基线和任务栈水位，然后在内部 RAM 中创建并销毁一个空 Kernel。它不驱动任何电机或 GPIO。这些日志是 `MA-001` 板级报告（[`reports/hardware/m0/board-feasibility.md`](../reports/hardware/m0/board-feasibility.md)）的输入，到板后的完整自检步骤也在该报告中。

**不得烧写 `EFUSE_STRAP_JTAG_SEL`。** GPIO3 用作急停检测输入并有外部上拉，烧写该 eFuse 后 GPIO3 会改变 JTAG 信号源（[`reports/hardware/m0/gpio-candidates.md`](../reports/hardware/m0/gpio-candidates.md)）。查看 eFuse 只用只读的 summary 命令。

## 结果分级

以下保留 M0 的验证分类；各 M1 PR 必须分别记录当次实际执行的配置、命令与结果，不能沿用这些历史结果作为新变更已通过的证明。按 AGENTS.md 与计划 §6 区分：

- **编译通过**：Native（GCC、Clang、ASan/UBSan、TSan）在本地与 CI 编译；ESP32-S3 固件由 CI 的 ESP-IDF 任务编译，本地未安装 ESP-IDF 时不编译。
- **模拟器运行**：`ayni-sim` 与全部 `ctest` 用例在 Linux 运行通过。
- **实机运行**：尚未进行，需要开发板。
- **长稳**：不适用于 M0。

## 内建策略（MA-009）

`strategies/builtin/` 仅链接公开 ABI，构建目标 `ayni_strategies`；相关测试目标 `ayni_strategies_tests`，用 `ctest -R '^(StrategiesFixture|Conversation|BuiltinRules|BuiltinLoop|CheckpointReaders)'` 选择。配置与对话保存顺序见组件 README。P2 的文件存储接入见 `runtime/native_node`；本地 Mock 测试不需要密钥或实 API。

### Native CLI（P2）

先按资源预算设置 `CMAKE_BUILD_PARALLEL_LEVEL` 与 `CTEST_PARALLEL_LEVEL`，再构建 `ayni` 目标：

```sh
cmake --preset native-debug
cmake --build --preset native-debug --target ayni ayni_native_assistant_tests
./build/native/targets/native_cli/ayni --mock
ctest --preset native-debug -R '^(NativeAssistant|KernelQuery|SimDemo|NativeCli)'
```

配置、授权与命令见 [CLI 使用说明](../targets/native_cli/README.md)。这是正常功能验证入口，CLI 测试通过 CTest 纳入 CI 的完整 ASan/UBSan 套件；真实服务由本机显式配置。目录默认在用户配置/数据位置，支持显式覆盖；`--help` 不读取密钥或启动 Kernel。

## C++ SDK 与公开示例

P5 提供源码级 `include/ayni/ayni.hpp` 和 [五个公开接入示例](../examples/README.md)（ADR-0067）。`ayni_cpp_sdk_smoke` 与示例客户端按 C++17、`-fno-exceptions -fno-rtti` 编译；当前 C ABI minor 为 0.52、Core schema 8；各阶段版本及兼容决定见对应 ADR。ESP-IDF 组合根包含同一 header，完整 CI 编译不能替代板级执行。

普通测试套件通过 CTest 运行 `CppSdk.*`、`cpp_sdk_without_exceptions_or_rtti`、`Examples.*`；`Examples.assistant_demo` 运行 Mock CLI 脚本。第二模型用本地 HTTP fixture，不使用真实服务或密钥。具体操作见示例 README；构建前仍按本页资源预算设置显式并行度。

内存受限平台可用 `Kernel::create_in` 提供已经量过的调用方 arena；自有 aligned new 路径仍须满足完整配置容量，不能据 Native facade 大小推算 ESP32 可用容量。

## P6 正常夹具

Native 构建时需要 Python 3.9+，`tools/generate_normal_fixtures.py` 校验 JSON 并生成 C++ 只读表。公开格式、动作和范围见 [正常夹具契约](../contracts/fixtures/normal/README.md)，架构边界见 ADR-0068。设置本页资源预算和正整数并行度后：

```sh
cmake --build --preset native-debug --target ayni-sim ayni_normal_fixture_tests
./build/native/targets/simulator/ayni-sim --list-normal
./build/native/targets/simulator/ayni-sim --case E1-N1 --output build/normal-E1-N1
ctest --preset native-debug -R 'NormalFixtures|normal_fixture_generator' --no-tests=error
```

输出目录必须尚不存在。每次保留 run.json 与 trace.jsonl，含实际配置、代码/工具链标记及第一次失败；只有静止点的正常排空/重开在本 profile 内。产物重放测试还验证 JSON、正文脱敏和两次完整轨迹一致；原故障/恢复动作明确拒绝。

## Native 容量与耗时测量

`tests/measurements` 是允许只读访问 Core 的测试工具，方法与限制见 ADR-0069。先按资源检查设置正整数构建/测试并行度；Release 无 sanitizer 的耗时才能与该基线比较：

```sh
cmake --preset native-release
cmake --build --preset native-release --target ayni_native_measurements ayni_memory_measurements
./build/native-release/tests/measurements/ayni_memory_measurements
./build/native-release/tests/measurements/ayni_native_measurements --maximum build/native-maximum.json
```

`--small OUTPUT.json` 用于正常功能 smoke。输出分别记录编码、管理提交、填充/排空、满表工作与静止 step 的样本数、最大值和 p99（ns）；Host 与计时均为真实单调时钟。`--maximum` 的所有配置容量取上限，但只填满 Goal、Task、Grant 和 Memory，其他占用如实列明；1 GiB 是逻辑模拟存储额度。最大观测值不能充当硬实时或 ESP32 证据；CI 的小负载不设性能阈值。

## 多节点正常模拟

构建 `ayni-sim` 后运行 `./build/native/targets/simulator/ayni-sim --multinode-normal --output NEW_DIRECTORY`，目录必须是新的。`MA038-N1` 的两个独立 Kernel 使用 typed FakeTransport，记录两次远程调用、两种查询、节点限定身份、独立时钟/持久前缀与在途正常关闭；产物为 `run.json` / `trace.jsonl`。它不读取线消息，不提供真实认证、持久重试映射或完整 MA-038 故障矩阵；细节与暂定服务 Task 选择见 ADR-0070。

## Native 覆盖率汇总

在干净源码和新的构建目录中，用固定 GCC 13、Debug、无 sanitizer 的同一挂载路径构建/执行。先检查资源并设置正整数并行度，再运行下列命令；`-fprofile-update=atomic` 用于并发测试中的计数更新：

```sh
cmake -S . -B build/native-coverage -G Ninja -DAYNI_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  '-DCMAKE_C_FLAGS=--coverage -fprofile-update=atomic' \
  '-DCMAKE_CXX_FLAGS=--coverage -fprofile-update=atomic' \
  -DCMAKE_EXE_LINKER_FLAGS=--coverage
cmake --build build/native-coverage
# 构建/测试发现完成后重置计数；只删除本轮构建目录的生成文件。
python3 -c 'from pathlib import Path; [p.unlink() for p in Path("build/native-coverage").rglob("*.gcda")]'
ctest --test-dir build/native-coverage --output-on-failure
./build/native-coverage/targets/simulator/ayni-sim
python3 tools/collect_coverage.py --build build/native-coverage \
  --raw build/native-coverage/gcov-json --output build/native-coverage-summary.json
```

原始目录和汇总文件必须是新的。收集器核对编译后二进制的源码修订/dirty、Debug/无 sanitizer 配置，拒绝关闭计数、非 atomic 与手工 sanitizer 的冲突参数，并按当前 `compile_commands.json` 收集全部有效 TU，忽略旧对象遗留的 notes。每个 TU 的原始 JSON 和 gcov 诊断单独保留；Core 或安全保守范围的 C/C++ 实现缺失时返回失败。行按路径/行号取覆盖并集；分支先去编译器 `throw` 弧，再仅在相同函数、相同有序 fallthrough/长度布局内合并，保留其它布局，不排除防御/恢复分支。

Core 和安全保守范围沿用 P4：`core/`、`control/permit_gate/`、`providers/mock/`、`providers/mock_execution/`，含全部 TU 中的相应头文件。缺少 gcov 条目的头文件单列供审查；未实例化模板/编译器未插桩的代码不能据此声称覆盖。工具结果不代表真实硬件或完整安全验收；未达目标的差距在本轮报告和 ADR 中记录。

## 完整实验运行设施

用户 2026-10-28 恢复完整 E2→E1→E3→E4 所需故障/恢复活动，变异继续暂停。先按本文检查资源并设置构建/测试并行度，再构建 `ayni-sim` 与 `ayni_experiment_tests`；当前 `--list-experiments` 列出 HARNESS 设施自验证及完整 E2/E1/E3/E4 Native/Sim 场景（#98～#101，ADR-0076～0079）；未注册的实验拒绝。

```sh
cmake --build --preset native-debug --target ayni-sim ayni_experiment_tests
./build/native/targets/simulator/ayni-sim --experiment HARNESS --output NEW_DIRECTORY
ctest --preset native-debug -R 'ExperimentFixtures|ExperimentEvidence|experiment_fixture_generator' --no-tests=error
```

受控动作、资源上限及证据范围见[完整实验 profile](../contracts/fixtures/experiments/README.md)与 ADR-0075。每例输出目录必须为新目录；运行设施通过不代表完整实验或硬件验收通过。

## AP-02 资源边界

ABI 0.48 的 `business_contract_max_bytes` 独立于 Scope 和业务正文，默认 4096 B、最大 69632 B；原证明分别随 Capability/Provider offer、Grant、Goal requirement、Call 保存，不能只在有 Call 后才留历史 Schema。公开 arena 查询包含这些容量，不包含 Host/Provider 堆、线程栈及第三方库内存。Native GCC 13 Debug 的本包前后 arena 对比、编译器栈帧及实际检查集中在 PR；不把最小 arena 数字当作平台总内存要求。

较大的证明上限会使自动 journal buffer 超过显式 `storage_buffer_bytes` 的 1 MiB 输入上限；自动配置按完整批次预算选择，显式小缓冲仍拒绝。`ayni_native_measurements` 的 small smoke 使用默认证明上限；maximum 保留全部容量/证明上限，结果中 `explicit_buffer_supported=false`、`accepted_buffer_min_bytes=0` 表示只可用自动缓冲，不能解释成 0 字节已通过准入。测量 Host 的逻辑介质容量为 4 GiB（不预分配），容纳最大 Schema-4 双快照及日志；该工具不是硬件容量承诺。

规范化/原证明解码仍使用有界栈暂存；本包在 Native 主线程验证，编译器静态单函数帧不是整条调用链峰值。ESP-IDF 栈、持久介质开销及真实 Host 资源支持尚未验证，不因 Native 正常回归推定可用。受限 Host 的配置与栈预算须在平台阶段实测。
