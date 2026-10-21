# 开发环境与命令

本文记录 M0（`MA-002`）及 M1 实际使用的环境与开发命令。每个 PR 的完整验证、独立审查和合并流程以 [`workflow.md`](workflow.md) 第 2、4、7 节为准；实际执行结果记在对应 ADR。开发环境以 Linux 为准；macOS 未经验证，不在此列出步骤。

## 已验证的版本

| 工具 | 版本 | 用途 |
|---|---|---|
| Ubuntu | 24.04.5 LTS，x86_64 | 本地与 CI（`ubuntu-24.04`） |
| CMake | M0：3.28.3；M1 容器：3.31.6（最低要求 3.22） | Native 构建 |
| Ninja | 1.11.1 | CMake 预设使用的生成器 |
| GCC | 13.3.0 | Native 构建，含 ASan/UBSan 与 TSan |
| Clang | 18.1.3 | Native 构建 |
| clang-format / clang-tidy | 18.1.3 | 格式检查（阻断）与静态分析（仅报告） |
| Python | 最低 3.9；M0 本地：3.13.16；M1 容器：3.12.3 | `tools/check_deps.py`、`tools/check_contracts.py` 与其测试 |
| GoogleTest | 1.17.0，按 SHA-256 锁定 | Native 单元测试（ADR-0012） |
| ESP-IDF | v6.0.3，工具链 `esp-15.2.0_20251204` | ESP32-S3 固件；CI 使用镜像 `espressif/idf:v6.0.3` |

M0 本地 Clang 缺少 ASan 运行库（`libclang-rt-18-dev`），当时只验证普通 Debug 构建；workflow 仍规定 GCC 的四个预设及 Clang Debug 分别验证。M1 本轮使用本机已有的 `ayni-m0-tools:latest` 容器，源码挂载到 `/workspace`，`CC=gcc-13`、`CXX=g++-13`；容器内实际版本为上表所列 CMake 3.31.6、GCC 13.3.0、Clang/clang-format 18.1.3、Ninja 1.11.1、Python 3.12.3。仓库没有该镜像的构建配方，复现以表中工具版本、预设和逐 PR 验证记录为准。

## 本轮验证的资源限制

`MA-010` E+F 后续本地验证统一串行执行：`CMAKE_BUILD_PARALLEL_LEVEL=1`，同一时间只运行一个构建或测试任务；不并行跑配置、植入缺陷或 Host 测试。验证容器设 1 CPU、2 GiB 内存硬上限、0 额外 swap；编译与测试进程使用较低的 CPU/I/O 优先级。开始前检查宿主可用内存；资源不足先停止任务，不用无界 `-j` 重试。逐项保存原始结果到仓库外的持久目录，避免机器重启后丢失缺陷恢复信息。

## Linux 准备

```sh
sudo apt-get install -y build-essential clang cmake ninja-build python3 \
    clang-format-18 clang-tidy-18 git
```

## Native 构建与测试

预设定义在 `CMakePresets.json`：

| 预设 | 构建目录 | 内容 |
|---|---|---|
| `native-debug` | `build/native` | Debug，含测试 |
| `native-release` | `build/native-release` | Release，含测试 |
| `native-asan` | `build/native-asan` | Debug，启用 AddressSanitizer 与 UndefinedBehaviorSanitizer |
| `native-tsan` | `build/native-tsan` | Debug，启用 ThreadSanitizer，覆盖入口队列与完成槽的多线程测试（ADR-0015） |

GCC 13 自带的 ThreadSanitizer 在 mmap 随机化为 32 位（`/proc/sys/vm/mmap_rnd_bits`）的内核上可能报 `unexpected memory mapping` 而无法启动，这时先执行 `sudo sysctl vm.mmap_rnd_bits=28`；CI 的 TSan 任务固定这样设置。M1 本轮先读到宿主值为 32，已按 workflow 在运行 TSan 前设置为 28；容器共享宿主内核，镜像本身不会改变这个参数。

```sh
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug
./build/native/targets/simulator/ayni-sim
```

不使用预设时，计划 §11 中的命令同样有效：

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
```

换用 Clang：在配置前设置 `CC=clang CXX=clang++`，并用 `-B` 指定另一个构建目录。只有带版本号的可执行文件时，使用对应的 `CC=clang-18 CXX=clang++-18`；GCC 同理可用 `CC=gcc-13 CXX=g++-13`。`AYNI_HOST` 目前只接受 `native`；ESP32 固件用下文的 `idf.py` 构建。`ayni-sim` 仍不接受参数，`--experiment E1` 等入口随 M1 的实验夹具加入；它每次运行在系统临时目录（`TMPDIR`，默认 `/tmp`）下新建状态目录，Native Host 的日志文件写在其中，Kernel 完成恢复并停机后删除该目录。Native Host 的单元测试与 Host 存储契约测试同样在临时目录中创建并删除存储文件，所以运行它们需要可写、支持 `fdatasync`、文件锁、rename 和目录同步的临时目录。状态目录包含 `commit_log`、双槽文件 `snapshot_0` / `snapshot_1` 及稳定锁文件 `.ayni_storage_lock`，轮转还会产生临时输出；保留设备状态时须保留整个目录。读取 schema 0 的迁移会在该目录发布 schema 1 快照，只有激活持久后才回收来源；不要仅恢复旧日志或旧快照作为已迁移状态的回滚办法（ADR-0027）。

### 离线配置 GoogleTest

首次配置测试会下载 GoogleTest 发布包。没有网络时，先把 `googletest-1.17.0.tar.gz` 解压到本地，再在配置时指定：

```sh
cmake --preset native-debug -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.17.0
```

## 依赖、契约、格式与静态分析

```sh
python3 tools/check_deps.py                                   # 依赖规则（ADR-0011）
python3 tools/check_contracts.py                              # 契约条款与追踪矩阵（ADR-0013）
git ls-files '*.c' '*.cpp' '*.h' | xargs clang-format-18 --dry-run --Werror
run-clang-tidy-18 -p build/native -quiet 'core/|hosts/|runtime/|targets/'
git diff --check
```

`ctest` 也会运行依赖检查、契约检查及其测试，并检查每个 Core 静态库不引用堆分配函数（ADR-0014）。修改 `contracts/` 中的条款后运行 `python3 tools/check_contracts.py --write` 重新生成 `contracts/traceability.md`，否则检查失败。clang-tidy 在 M0 只报告，不阻断 CI。

## ESP32-S3 固件

安装锁定版本的 ESP-IDF（只需一次）：

```sh
git clone --branch v6.0.3 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
```

每个终端先激活环境，再构建：

```sh
. ~/esp/esp-idf/export.sh
cd targets/esp32_assistant
idf.py build
idf.py size
```

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
