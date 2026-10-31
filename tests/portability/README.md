# 独立工程接入与平台验证

本目录是同仓库独立消费工程，不使用根部署工程、Core私有头或内部runtime fixture。当前先交付`sdk_import`；完整运行、平台适配和共同场景的进度见[实施计划](../../docs/architecture/independent-consumption-plan.md)。

按资源预算设置正整数`CMAKE_BUILD_PARALLEL_LEVEL`和`CTEST_PARALLEL_LEVEL`后：

```sh
python3 tests/portability/sdk_import_test.py --source . --optional
```

运行器默认使用Ninja，也可传`--generator`明确选择；CTest传递父工程生成器。运行器把必要SDK和消费者复制到临时独立目录，将SDK设为只读，配置/构建/运行后清理。基础SDK禁用SQLite、CURL、Threads查找；显式Mock组件使用现有锁版yyjson。失败打印实际子进程输出。需要复用下载缓存时传`--download-dir PATH`；sanitizer由`--sanitize address,undefined`明确传递，CI使用父配置相同值。

通过只表示公开组件的独立导入与Native探针成功；不代表ESP运行、存储恢复或完整Core机制验收通过。
