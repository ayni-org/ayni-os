# ADR-0094：共同会话运行时与独立正常程序

- **状态**：已接受范围；实现与验证见 [#123](https://github.com/RobotSe7en/ayni/pull/123)。
- **日期**：2026-10-31。
- **关联**：ADR-0089～0093；[独立接入计划](../architecture/independent-consumption-plan.md)、[独立程序](../../tests/portability/README.md)。

## 背景

源码导入、低栈工作区及真实Host存储已交付。独立工程仍需共同ToolLoop应用、正文与Core锚点的确认闭环，以及可在Native运行、在ESP完整编译的同一正常程序。只链接Core或启动Hello不能提供这些证据。

## 决策

`runtime/conversation`拥有有界会话正文、独立文件CAS版本和异步完成；Core继续拥有Goal/Task、ToolLoop调度、来源与隐私版本。新输入先保存独立draft，再提交Goal；完成Task先发布Core锚点，正文实际同步后ACK，ACK持久后才公开答复。每次公开读取/输入核对当前Core锚点。已有未完成跨存储状态或版本冲突明确拒绝并保留；不在本包增加恢复算法。

Task的暂停/授权/模型限额由Core解释。会话报告hold状态并保持原Task连接，owner处理后继续；一个Agent的hold不阻止另一个Agent的正文/ACK进展。文件错误或无法协调的锚点冲突不伪造完成。关闭先停止Core并排空Provider，再join Host，最后destroy及释放外部区域。

`runtime/portable`是可选组合：两个同语义模型实例、各自Binding实际资源、公共摘要服务、软件长作业/标量订阅及模拟许可执行器。确定性模型只提议调用，实际求和由软件Provider执行；它不是联网模型质量证据。Host可注入非阻塞UTC来源，默认Native系统时钟和ESP未提供UTC的行为保持。

同一逻辑owner顺序调用ToolLoop、Conversation codec和值操作，复用三者公开query的最大区域；回调内嵌套操作仍需独立scratch，worker只借用独立文件缓冲。调用方区域query不含Host与既有Provider启动时有界自有堆。没有Core平台分支、全局工作区或持久scratch指针。

独立消费者位于`tests/portability/`，只使用公开API/组件。源码运行器复制声明的SDK闭包并在只读依赖旁构建消费者，Native不导入SQLite/CURL/内部部署或测试fixture。ESP用锁版IDF/LittleFS、16MiB Flash/8MiB PSRAM、专用8MiB卷；PSRAM XiP保证异步Flash操作期间的访问前提，Host/owner/worker栈仍在内部RAM。默认不格式化，显式选项只初始化全FF卷；非空旧验收数据拒绝作为新空白用例运行。

公开C ABI0.53、Core schema8、Conversation codec4和SQLite2不变。新增Host/运行时是源码组件，消费者重新编译。Ayni实现显式使用C++17，覆盖IDF6.0.3的C++26默认参数；修复uint32_t与unsigned/int同宽异型的模板、格式使用，未加入ESP分支。

## 后果

换Host只改消费者的runtime组合及物理内存分配，共同应用/场景不改。可独立验证工作、权限、资源、目录/结构值、受管操作、记忆/上下文、日历、存储及模拟执行。正常重开核对实际正文与原调用结果，不重新调用模型或执行器。

96KiB owner及24KiB worker栈配置不是实测余量；程序只有实际运行并观察两者至少25%空余才通过相应断言。Flash计数是本次正常程序的整片driver调用/字节/时间，不是单元磨损或耐久结论。

## 备选方案

不复制Core/ToolLoop到设备工程，不将Host文件实现引入applications，不把fixture/厂商分支加入Core，不用虚构文件ACK代替持久事实。共享scratch基于已验证的单owner同步调用链；不增加通用插件、平台工厂或全局服务注册。

## 验证

实际Native、隔离导入、ESP编译与独立复查结果集中在 [#123](https://github.com/RobotSe7en/ayni/pull/123)。实机、实际栈/Flash、掉电、耐久及其他新增故障/变异专项仍未验证。
