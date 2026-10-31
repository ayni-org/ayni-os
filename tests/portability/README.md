# 独立工程接入与平台验证

本目录的消费者有自己的CMake入口，导入Ayni源码SDK，不导入根部署、Core私有头或内部测试fixture。`common/suite.cpp`在两端编译为同一应用；消费者入口负责选择Host及物理内存，Core与ToolLoop实现不分叉。可选SDK组件`fixture_model`、`conversation`、`portable_profile`分别导出`Ayni::FixtureModel`、`Ayni::Conversation`、`Ayni::PortableProfile`；默认Core/Strategies导入不选择这些组合。

## Native独立运行

按机器资源设置正整数并行度，再使用锁版工具链：

```sh
export CMAKE_BUILD_PARALLEL_LEVEL=4 CTEST_PARALLEL_LEVEL=4
python3 tests/portability/run_native.py --source . --source-revision "$(git rev-parse HEAD)"
```

运行器复制声明的SDK闭包并设为只读，在独立目录构建/运行，禁用SQLite/CURL，并检查消费者没有Core私有include。原始探针仍可用`python3 tests/portability/sdk_import_test.py --source . --optional`。生成器用`--generator`明确选择，sanitizer通过`--sanitize`传递；CTest继承构建类型、sanitizer、生成器和环境CC/CXX；仅在父CMake显式选择编译器时，也应相应设置CC/CXX。版本标签为调用者提供的源码修订；`working-tree/unversioned`不构成冻结版本证据。

完整程序正常关闭并重开同一真实文件目录，核对Agent/Goal/Call身份、结构结果、正文及摘要/历史，并确认没有模型/执行器重派。文本指纹仅为内存中的正常用例比较，不是安全证明或持久hash清单。两槽快照由实际来源删除/隐私压缩产生。不是掉电或故障恢复专项。

## ESP32-S3独立编译

激活ESP-IDF6.0.3，输出目录须不存在：

```sh
python3 tests/portability/build_esp.py --source . --output build/independent-esp \
  --source-revision "$(git rev-parse HEAD)"
```

运行器同样复制只读SDK与独立消费者。先`idf.py reconfigure`，再以`cmake --build --parallel`限制实际构建并行度；产物在输出目录的`build/`。可直接从`esp32_s3/`配置`AYNI_SOURCE_DIR`构建。配置与锁文件独立，不读取旧Hello工程。

固定N16R8、4MiB应用分区、8MiB专用LittleFS卷；启动检查实际Flash/PSRAM。调用方大区域放PSRAM，Host与96KiB owner/24KiB worker栈放内部RAM；[IDF6.0.3 PSRAM XiP](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-guides/external-ram.html#execute-in-place-xip-from-psram)同时启用指令/只读数据搬移，维持Flash操作期间访问前提。默认`AYNI_INITIALIZE_BLANK=n`，只在明确选择后初始化完全擦空的卷；未知非空数据不格式化。重复运行已有完成数据会拒绝作为新用例，保留目录。

程序包含实际FreeRTOS owner循环、Host后台I/O、全部共同场景及正常重开；不是Hello链接探针。只有实机执行后，才记录实际查询字节数、内部/PSRAM堆及最大块/最小余量、两栈高水位，并要求至少25%栈空余。Flash driver调用次数、字节及时间在worker join后采集，覆盖本次全芯片调用，不代表擦除单元磨损或耐久。

当前只授权代码与编译，不执行烧录、板级操作或付费模型。`targets/esp32_assistant`保留为板级诊断，现只报告平台/ABI/arena query，不再尝试把通用默认Kernel放入内部RAM，也不提供生命周期验收。

## 正常场景

| 共同场景 | 实际检查 |
|---|---|
| 工作与生命周期 | delay/interval/local input、暂停组、可信UTC日历、完整停止 |
| 权限 | proposal/owner approval、委派、撤销及新请求缺权 |
| 目录与结构值 | 真实usage/schema检查，512项求和结果精确为131328 |
| 资源与多实例 | 相同能力的A/B绑定使用model.a/model.b，逻辑model.slot不占用；最多4个存活Task |
| 长作业/订阅 | 同Task的真实作业、stream、有效scalar样本与关闭 |
| 记忆/上下文 | USER→DERIVED→确认→来源删除，真实正文擦除/ACK及隐私完成 |
| ToolLoop | 四工具协议提议、真实工具结果、20个短轮次跨摘要阈值 |
| 可恢复hold | soft上限暂停，B仍可完成；owner提高上限后同Task继续，8次sum不重复 |
| 执行许可 | 模拟Scene、真实permit gate、输出及独立停止事实；无物理输出 |
| 持久化 | 实际两槽压缩、日志、正文与正常关闭/重开；不重发旧调用 |

`esp_files/`另为生产LittleFiles+真实锁版LittleFS的RAM NOR正常检查，不运行FreeRTOS。Native运行、RAM介质、ESP编译、ESP实机分别报告；编译不能判定实际Flash、栈或板级验收通过。
