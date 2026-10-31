# 独立源码组件与共同正常程序交付

- 基线与范围：源码SDK #118、有界工作区 #119、共享存储 #120、ESP Host #121、外层值 #122，以及共同程序 #123；ABI0.53/schema8。
- 状态：#118～#123 已合并，独立验证前所需代码已交付。只读源码SDK隔离导入、Native共同正常程序及完整CI通过；ESP32-S3完成编译和链接，平台实机未执行。实际证据集中在[#123](https://github.com/RobotSe7en/ayni/pull/123)。
- 复现与场景：[独立程序说明](../../tests/portability/README.md)、[实施计划](../../docs/architecture/independent-consumption-plan.md)、ADR-0094。

同一Core和ToolLoop可从源码SDK独立导入。共同程序覆盖工作/生命周期、权限/审批/委派/撤销、目录/结构结果、实例资源映射、长作业/订阅/标量来源、记忆/正文与隐私、UTC日历、真实存储/两槽压缩、模拟执行许可和正常关闭重开。持有Core可恢复hold时仍推进另一Agent，owner处理后继续原Task，不重发工具。重开比较实际正文及原调用结果，不以计数代替内容。

平台只选择Host、端口和物理内存；屏幕、声音、模型网络服务及电机驱动不进入Core。默认导入不强迫选择独立验收profile或其确定性夹具。Profile调用方query、既有Provider启动堆和Host/任务栈分别预算；同owner同步scratch共享不扩大到worker或嵌套回调。

整部分收尾覆盖率达到Core行≥75%、安全关键分支≥80%的现行目标。统计仍覆盖全部Core、permit gate和既有mock实现，保持非throw弧及有序布局并集口径；精确分子、分母、编译配置与限制见[#123](https://github.com/RobotSe7en/ayni/pull/123)。未实例化或未插桩头文件不据此声明已覆盖。

尚无ESP实际堆/栈、FreeRTOS时序、真实Flash写入量、板级/外设或耐久结果。程序已准备内部/PSRAM堆统计、两任务栈25%余量判断及Flash driver计数，编译不能判定这些指标通过。正常关闭重开未扩成掉电、变异或新增故障专项；Linux系统部署、ESP实机及付费模型仍按后续实验授权分别进行。
